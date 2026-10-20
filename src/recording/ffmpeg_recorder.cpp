// ============================ Recording (ffmpeg subprocess) ============================
//
// Per-stream recorder. A dedicated writer thread snapshots the already-converted BGRA
// frame published for `index` (the same buffer Unity displays), paces it at a constant
// fps, and pipes raw BGRA to an ffmpeg.exe child process. ffmpeg owns segmentation, so
// each .mov file is exactly fps*segmentSeconds frames (= exactly 1 minute by default).
// `recordedFrame` is the synchronization currency Unity uses to key its .srt metadata.

#include "unityDeltacast_internal.hpp"
#include "recording/ffmpeg_command.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>      // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")

using namespace deltacast;

struct RecorderCtx {
    std::atomic<bool> recording{ false };
    std::thread thread;
    std::atomic<unsigned long long> recordedFrame{ 0 };

    // Set at StartRecording, read by the writer thread it then starts; the resolution is
    // locked once the first frame is available.
    FfmpegRecording config;

    // A writer that stopped on its own is still joinable; destroying it unjoined would
    // std::terminate the host process at exit (same rule as CaptureWorker).
    ~RecorderCtx() {
        recording.store(false);
        if (thread.joinable()) thread.join();
    }
};

static RecorderCtx recorders[4];
static std::mutex recordingLifecycle[4];

// Write the whole buffer to the pipe, tolerating partial writes.
static bool WriteAllToPipe(HANDLE h, const uint8_t* data, size_t size)
{
    size_t off = 0;
    while (off < size) {
        DWORD chunk = (DWORD)std::min<size_t>(size - off, (size_t)(1u << 20));
        DWORD written = 0;
        if (!WriteFile(h, data + off, chunk, &written, nullptr) || written == 0) {
            return false;
        }
        off += written;
    }
    return true;
}

// Launch ffmpeg with stdin = read end of a pipe and stderr/stdout -> a per-stream log file.
// On success, fills hStdInWr (write end, owned by caller) and pi.
static bool SpawnFfmpeg(RecorderCtx& r, HANDLE& hStdInWr, PROCESS_INFORMATION& pi)
{
    // Inheritable handles leak into every process created while they exist. Serialize the
    // recorders' spawns so no ffmpeg holds another stream's pipe: that ffmpeg would keep the
    // pipe alive, so a dead encoder would block its writer instead of failing it.
    static std::mutex spawnMutex;
    std::lock_guard<std::mutex> spawnLock(spawnMutex);

    hStdInWr = nullptr;
    ZeroMemory(&pi, sizeof(pi));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    // Create both ends non-inheritable; only the child's read end is made inheritable, so
    // the write end (whose last copy closing signals end-of-input) never leaks.
    HANDLE hStdInRd = nullptr;
    if (!CreatePipe(&hStdInRd, &hStdInWr, nullptr, 0)) {
        DC_LOG("rec: CreatePipe failed err=" + std::to_string(GetLastError()));
        return false;
    }
    SetHandleInformation(hStdInRd, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

    // ffmpeg's diagnostics go to a log file next to the output (a file, not a pipe, so it
    // can never deadlock on a full pipe). Fall back to NUL if the log can't be created.
    // Appended, so a recording resumed after a reconnect keeps the earlier run's log.
    const std::string logPath = FfmpegLogPath(r.config.outputPattern);
    HANDLE hLog = CreateFileA(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hLog == INVALID_HANDLE_VALUE) {
        hLog = CreateFileA("NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           &sa, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = hStdInRd;
    si.hStdOutput = hLog;
    si.hStdError = hLog;

    const std::string cmd = BuildFfmpegCommand(r.config);
    DC_LOG(std::string("rec: launching ffmpeg: ") + cmd);

    // CreateProcessA needs a writable command-line buffer.
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');

    BOOL ok = CreateProcessA(
        nullptr,            // lpApplicationName null -> search PATH using cmdline's first token
        cmdline.data(),
        nullptr, nullptr,
        TRUE,               // inherit handles (pipe read end + log file)
        CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    // Parent's copies of the handles handed to the child.
    CloseHandle(hStdInRd);
    if (hLog != INVALID_HANDLE_VALUE) CloseHandle(hLog);

    if (!ok) {
        DC_LOG("rec: CreateProcess(ffmpeg) failed err=" + std::to_string(GetLastError()));
        CloseHandle(hStdInWr);
        hStdInWr = nullptr;
        return false;
    }
    return true;
}

static void RecordWriterLoop(int index)
{
    RecorderCtx& r = recorders[index];

    // 1) Wait for capture to publish a frame so we know the resolution.
    while (r.recording.load(std::memory_order_relaxed)) {
        int w = width[index].load();
        int h = height[index].load();
        if (w > 0 && h > 0) { r.config.width = w; r.config.height = h; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!r.recording.load(std::memory_order_relaxed)) return;

    const size_t frameBytes = size_t(r.config.width) * size_t(r.config.height) * 4;

    // 2) Launch ffmpeg now that -video_size is known.
    HANDLE hStdInWr = nullptr;
    PROCESS_INFORMATION pi{};
    if (!SpawnFfmpeg(r, hStdInWr, pi)) {
        r.recording.store(false, std::memory_order_relaxed);
        return;
    }

    // 3) Constant-fps write loop with gap-fill (repeat last frame) to hold the rate.
    timeBeginPeriod(1);
    const auto t0 = std::chrono::steady_clock::now();
    const double interval = 1.0 / double(r.config.fps);

    std::vector<uint8_t> last;   // most recent snapshot; reused as the gap-fill source
    bool haveLast = false;
    uint64_t lastCapturedSeen = 0;  // last capture-frame counter we copied
    unsigned long long nextIdx = 0;
    bool resolutionChanged = false;

    while (r.recording.load(std::memory_order_relaxed)) {
        const auto target = t0 + std::chrono::nanoseconds(
            (long long)(double(nextIdx) * interval * 1e9));
        std::this_thread::sleep_until(target);

        const auto snapshot = publishedFrames[index].copy_new(lastCapturedSeen, frameBytes, last);
        if (snapshot == SnapshotResult::Copied) haveLast = true;
        if (snapshot == SnapshotResult::SizeChanged) resolutionChanged = true;

        if (resolutionChanged) {
            DC_LOG("rec: resolution changed mid-recording, stopping index=" + std::to_string(index));
            break;
        }

        if (!haveLast) {
            // No frame captured yet; don't advance the frame index (keeps fps honest).
            continue;
        }

        if (!WriteAllToPipe(hStdInWr, last.data(), last.size())) {
            DC_LOG("rec: pipe write failed (ffmpeg gone?) index=" + std::to_string(index));
            break;
        }
        r.recordedFrame.fetch_add(1, std::memory_order_relaxed);
        ++nextIdx;
    }

    timeEndPeriod(1);

    // 4) Teardown: closing stdin tells ffmpeg to flush and finalize the last segment.
    if (hStdInWr) { CloseHandle(hStdInWr); hStdInWr = nullptr; }
    if (pi.hProcess) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        CloseHandle(pi.hProcess);
    }
    if (pi.hThread) CloseHandle(pi.hThread);

    r.recording.store(false, std::memory_order_relaxed);
    DC_LOG("rec: writer finished index=" + std::to_string(index)
        + " frames=" + std::to_string(r.recordedFrame.load()));
}


extern "C" {

    UNITYDLL_EXPORT int StartRecording(int index,
                                       const char* ffmpegExe,
                                       const char* outputPattern,
                                       int fps,
                                       int segmentSeconds,
                                       int firstPart,
                                       const char* encoderArgs,
                                       int applyVFlip)
    {
        if (!valid_index(index)) return 0;
        if (!outputPattern || !*outputPattern || firstPart < 0) return 0;
        if (fps <= 0) fps = 30;
        if (segmentSeconds <= 0) segmentSeconds = 60;

        std::lock_guard<std::mutex> lifecycle(recordingLifecycle[index]);
        RecorderCtx& r = recorders[index];

        bool expected = false;
        if (!r.recording.compare_exchange_strong(expected, true)) {
            return 0; // already recording
        }

        try {
        // A previous session may have stopped on its own (e.g. resolution change) without
        // StopRecording() being called yet; join its finished thread before reusing the slot.
        if (r.thread.joinable()) r.thread.join();

        r.recordedFrame.store(0, std::memory_order_relaxed);
        r.config.ffmpegExe = (ffmpegExe && *ffmpegExe) ? ffmpegExe : "ffmpeg";
        r.config.outputPattern = outputPattern;
        r.config.encoderArgs = encoderArgs ? encoderArgs : "";
        r.config.fps = fps;
        r.config.segmentSeconds = segmentSeconds;
        r.config.firstPart = firstPart;
        r.config.vflip = (applyVFlip != 0);
        r.config.width = 0;
        r.config.height = 0;

        r.thread = std::thread(RecordWriterLoop, index);
        return 1;
        } catch (...) {
            r.recording.store(false);
            return 0;
        }
    }

    UNITYDLL_EXPORT void StopRecording(int index)
    {
        if (!valid_index(index)) return;
        std::lock_guard<std::mutex> lifecycle(recordingLifecycle[index]);
        RecorderCtx& r = recorders[index];
        r.recording.store(false, std::memory_order_relaxed);
        if (r.thread.joinable()) r.thread.join();
    }

    UNITYDLL_EXPORT int IsRecording(int index)
    {
        if (!valid_index(index)) return 0;
        return recorders[index].recording.load(std::memory_order_relaxed) ? 1 : 0;
    }

    UNITYDLL_EXPORT unsigned long long GetRecordedFrameNumber(int index)
    {
        if (!valid_index(index)) return 0;
        return recorders[index].recordedFrame.load(std::memory_order_relaxed);
    }

} // extern "C"
