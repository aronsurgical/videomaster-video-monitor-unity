#include "unityDeltacast.h"
#include "capture_state.hpp"
#include "pixel_convert.hpp"

#include <string>
#include <atomic>
#include <thread>
#include <variant>
#include <mutex>
#include <vector>
#include <algorithm>
#include <chrono>
#include <sstream>
#include <array>
#include <cstring>
#include <initializer_list>

// NOMINMAX must be set before any windows.h inclusion so the min/max macros don't clobber
// the std::min<> used in this file. windows.h itself is included AFTER the VideoMaster headers
// below (see note there), so define the guard now but defer the include.
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <VideoMasterCppApi/exception.hpp>
#include <VideoMasterCppApi/api.hpp>
#include <VideoMasterCppApi/board/board.hpp>
#include <VideoMasterCppApi/stream/sdi/sdi_stream.hpp>
#include <VideoMasterCppApi/slot/sdi/sdi_slot.hpp>
#include <VideoMasterCppApi/helper/sdi.hpp>

#include "../src/helper.hpp"

// Windows process/pipe API for the offloaded ffmpeg recording. Included AFTER the VideoMaster
// headers on purpose: windows.h #defines `interface` (-> struct) and `GetMessage`, which would
// otherwise break VideoMaster's headers and collide with our exported GetMessage(). Undef
// GetMessage right after the include so our exported symbol keeps its name.
#include <windows.h>
#include <mmsystem.h>      // timeBeginPeriod / timeEndPeriod
#pragma comment(lib, "winmm.lib")
#ifdef GetMessage
#undef GetMessage
#endif

// Statement-like macro with file, line, and function
#define DC_LOG(MSG)                                                        \
    do {                                                                   \
        logHelper( std::string("[") + __FILE__ + ":" +                     \
                   std::to_string(__LINE__) + " " + __func__ + "] " +      \
                   std::string(MSG) );                                     \
    } while (0)


using namespace Application::Helper;

using namespace Deltacast::Wrapper;


static deltacast::PublishedFrame publishedFrames[4];

// simple static message buffer
static std::string message = "Hello Deltacast DLL!\n";
static std::mutex  messageMutex;

static std::array<std::atomic<int>, 4> width = {
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0}
};

static std::array<std::atomic<int>, 4> height = {
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0}
};

static std::array<std::atomic<bool>, 4> burnInFrameNumber = {
    std::atomic<bool>{false},
    std::atomic<bool>{false},
    std::atomic<bool>{false},
    std::atomic<bool>{false}
};



// Per-stream human-readable signal/video description, exposed via GetSignalAndVideoInfo().
// (Restored from commit 9a1e1a8, which was dropped by a later merge.)
static std::array<std::string, 4> videoInfo;
static std::array<std::mutex, 4>  videoInfoMutex;

static void SetVideoInfo(int index, std::string text) {
    if (index < 0 || index >= (int)videoInfo.size()) return;
    std::lock_guard<std::mutex> lk(videoInfoMutex[index]);
    videoInfo[index] = std::move(text);
}

// A 1080i50 Level-B dual-stream signal contains 50 temporally distinct fields
// per second, but only 25 complete interlaced frames. A fieldMerge value of 0
// selects field mode for this specific signal type so each field can be
// published as its own bob-deinterlaced, full-height frame. Other signal types
// retain the old fieldMerge == 0 behavior.
static bool ShouldUseFieldModeBob(const SignalInformation& signalInformation,
                                  unsigned int fieldMerge)
{
    if (fieldMerge != UNITYDELTACAST_FIELD_MODE_BOB) return false;

    const auto* sdi = std::get_if<SdiSignalInformation>(&signalInformation);
    if (!sdi
        || sdi->video_interface != VHD_INTERFACE_3G_B_DS_425_1
        || sdi->video_standard != VHD_VIDEOSTD_S274M_1080i_50Hz) return false;

    return Application::Helper::get_video_characteristics(signalInformation).interlaced != 0;
}


static void logHelper(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(messageMutex);
    if (message.size() < 2000) {
        message += msg + "\n";
    }
}


// ============================ Recording (ffmpeg subprocess) ============================
//
// Per-stream recorder. A dedicated writer thread snapshots the already-converted BGRA
// frame published for `index` (the same buffer Unity displays), paces it at a constant
// fps, and pipes raw BGRA to an ffmpeg.exe child process. ffmpeg owns segmentation, so
// each .mov file is exactly fps*segmentSeconds frames (= exactly 1 minute by default).
// `recordedFrame` is the synchronization currency Unity uses to key its .srt metadata.

struct RecorderCtx {
    std::atomic<bool> recording{ false };
    std::thread thread;
    std::atomic<unsigned long long> recordedFrame{ 0 };

    // configuration captured at StartRecording (read by the writer thread after it starts,
    // which is synchronized by the std::thread construction that follows the writes)
    std::string ffmpegExe;
    std::string outputPattern;
    std::string encoderArgs;
    int fps = 30;
    int segmentSeconds = 60;
    bool vflip = true;

    // resolution locked once the first frame is available
    int width = 0;
    int height = 0;

    // A writer that stopped on its own is still joinable; destroying it unjoined would
    // std::terminate the host process at exit (same rule as CaptureWorker).
    ~RecorderCtx() {
        recording.store(false);
        if (thread.joinable()) thread.join();
    }
};

static RecorderCtx recorders[4];
static std::mutex recordingLifecycle[4];
static deltacast::CaptureWorker captureWorkers[4];

// Assemble the ffmpeg argument string, identical to the known-good Unity command.
static std::string BuildFfmpegCommand(const RecorderCtx& r)
{
    const std::string defaultEncoder =
        "-c:v h264_nvenc -preset p1 -rc vbr -cq 22 -pix_fmt yuv420p -g "
        + std::to_string(r.fps) + " -rgb_mode yuv420";
    const std::string& encoder = r.encoderArgs.empty() ? defaultEncoder : r.encoderArgs;
    // Encode RGB back to YUV with the matrix the frames were decoded with, and tag it.
    const bool hd = deltacast::IsHdHeight(r.height);

    std::ostringstream cmd;
    cmd << "\"" << r.ffmpegExe << "\""
        << " -y -f rawvideo -pixel_format bgra"
        << " -video_size " << r.width << "x" << r.height
        << " -framerate " << r.fps
        << " -i -"
        << " -vf " << (r.vflip ? "vflip," : "")
        << "scale=out_color_matrix=" << (hd ? "bt709" : "bt601");
    cmd << " " << encoder
        << " -colorspace " << (hd ? "bt709" : "smpte170m")
        // segments are cut at keyframes; force one at every boundary even for custom encoder args
        << " -force_key_frames expr:gte(t,n_forced*" << r.segmentSeconds << ")"
        << " -f segment -segment_time " << r.segmentSeconds
        << " -reset_timestamps 1 -segment_format mov"
        << " \"" << r.outputPattern << "\"";
    return cmd.str();
}

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
    const std::string logPath = r.outputPattern + ".ffmpeg.log";
    HANDLE hLog = CreateFileA(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

    const std::string cmd = BuildFfmpegCommand(r);
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
        if (w > 0 && h > 0) { r.width = w; r.height = h; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!r.recording.load(std::memory_order_relaxed)) return;

    const size_t frameBytes = size_t(r.width) * size_t(r.height) * 4;

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
    const double interval = 1.0 / double(r.fps);

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
        if (snapshot == deltacast::SnapshotResult::Copied) haveLast = true;
        if (snapshot == deltacast::SnapshotResult::SizeChanged) resolutionChanged = true;

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


// ============================ Capture ============================

// Clears what Unity and the recorder read for `index`, so a stopped or restarting stream
// never serves the previous session's frame or resolution.
static void ResetStream(int index, const char* status)
{
    publishedFrames[index].reset();
    width[index].store(0);
    height[index].store(0);
    SetVideoInfo(index, status);
}

template<class Work>
static void StartWorker(int index, Work work)
{
    try {
        captureWorkers[index].start([index] { ResetStream(index, "Starting"); }, std::move(work));
    } catch (const std::exception& e) {
        SetVideoInfo(index, std::string("Capture start failed: ") + e.what());
    } catch (...) {
        SetVideoInfo(index, "Capture start failed");
    }
}

// Runs capture sessions until StopCapture. Any SDK error (cable pulled, slot timeout,
// failed detection) ends only the current session; the next one starts after a short pause.
template<class Session>
static void RunCaptureSessions(int index, Session session)
{
    auto& running = captureWorkers[index].running;
    while (running.load()) {
        try {
            session();
        } catch (const std::exception& e) {
            SetVideoInfo(index, std::string("Capture error, retrying: ") + e.what());
            DC_LOG(std::string("capture error: ") + e.what());
            for (int i = 0; i < 5 && running.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

// Waits until the connector carries a signal; false once StopCapture was requested.
// (Application::Helper::wait_for_input expects a stop flag, not the running flag.)
static bool WaitForSignal(int index, BoardComponents::RxConnector& rx)
{
    if (rx.signal_present()) return true;
    SetVideoInfo(index, "No signal");
    while (captureWorkers[index].running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (rx.signal_present()) return true;
    }
    return false;
}

// Burns the next publication number into the top-right corner of each {x, width} region of
// the frame (when enabled), then publishes it.
static void PublishFrame(int index, std::vector<uint8_t>& frame, int W, int H,
                         std::initializer_list<std::pair<int, int>> regions)
{
    if (burnInFrameNumber[index].load(std::memory_order_relaxed)) {
        const unsigned long long frameNo = publishedFrames[index].sequence() + 1;
        for (const auto& [x, w] : regions)
            deltacast::BurnFrameNumberBGRA(frame.data(), W, H, W * 4, frameNo, x, w, true);
    }
    publishedFrames[index].publish(frame);
}


extern "C" {

    // Unity calls this first; it doubles as the "is the DLL present" probe.
    UNITYDLL_EXPORT void InitLibrary() {
        DC_LOG("Library initialized!");
    }

    UNITYDLL_EXPORT const char* GetMessage() {
        thread_local std::string last;
        std::lock_guard<std::mutex> lk(messageMutex);
        last = message;
        message.clear();
        return last.c_str(); // 'last' persists between calls
    }

    UNITYDLL_EXPORT int GetWidth(int index) {
        return deltacast::valid_index(index) ? width[index].load() : 0;
    }

    UNITYDLL_EXPORT int GetHeight(int index) {
        return deltacast::valid_index(index) ? height[index].load() : 0;
    }

    UNITYDLL_EXPORT void SetBurnInFrameNumber(int index, int enabled)
    {
        if (!deltacast::valid_index(index)) return;

        burnInFrameNumber[index].store(enabled != 0, std::memory_order_relaxed);
    }

    UNITYDLL_EXPORT unsigned long long GetNativeFrameCounter(int index)
    {
        if (!deltacast::valid_index(index)) return 0;

        return publishedFrames[index].sequence();
    }

    UNITYDLL_EXPORT void GetSignalAndVideoInfo(int index, char* buffer, int bufferSize) {
        if (!deltacast::valid_index(index) || !buffer || bufferSize <= 0)
            return;

        std::lock_guard<std::mutex> lk(videoInfoMutex[index]);

        strncpy(buffer, videoInfo[index].c_str(), bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }

    UNITYDLL_EXPORT int StartRecording(int index,
                                       const char* ffmpegExe,
                                       const char* outputPattern,
                                       int fps,
                                       int segmentSeconds,
                                       const char* encoderArgs,
                                       int applyVFlip)
    {
        if (!deltacast::valid_index(index)) return 0;
        if (!outputPattern || !*outputPattern) return 0;
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
        r.ffmpegExe = (ffmpegExe && *ffmpegExe) ? ffmpegExe : "ffmpeg";
        r.outputPattern = outputPattern;
        r.encoderArgs = encoderArgs ? encoderArgs : "";
        r.fps = fps;
        r.segmentSeconds = segmentSeconds;
        r.vflip = (applyVFlip != 0);
        r.width = 0;
        r.height = 0;

        r.thread = std::thread(RecordWriterLoop, index);
        return 1;
        } catch (...) {
            r.recording.store(false);
            return 0;
        }
    }

    UNITYDLL_EXPORT void StopRecording(int index)
    {
        if (!deltacast::valid_index(index)) return;
        std::lock_guard<std::mutex> lifecycle(recordingLifecycle[index]);
        RecorderCtx& r = recorders[index];
        r.recording.store(false, std::memory_order_relaxed);
        if (r.thread.joinable()) r.thread.join();
    }

    UNITYDLL_EXPORT int IsRecording(int index)
    {
        if (!deltacast::valid_index(index)) return 0;
        return recorders[index].recording.load(std::memory_order_relaxed) ? 1 : 0;
    }

    UNITYDLL_EXPORT unsigned long long GetRecordedFrameNumber(int index)
    {
        if (!deltacast::valid_index(index)) return 0;
        return recorders[index].recordedFrame.load(std::memory_order_relaxed);
    }


UNITYDLL_EXPORT void StartCapture(
    int index,
    int device_id,
    int rx_stream_id,
    int buffer_depth,
    char inputType,
    unsigned int requested_width,
    unsigned int requested_height,
    unsigned int progressive,
    unsigned int framerate,
    VHD_DV_CS cable_color_space,
    VHD_DV_SAMPLING cable_sampling,
    VHD_VIDEOSTANDARD video_standard,
    VHD_CLOCKDIVISOR clock_divisor,
    VHD_INTERFACE video_interface,
    unsigned int fieldMerge,
    VHD_BUFFERPACKING buffer_packing)
{
    if (!deltacast::valid_index(index)) {
        return;
    }

    if (buffer_packing != VHD_BUFPACK_VIDEO_YUV422_8) {
        SetVideoInfo(index, "Unsupported buffer packing: UYVY8 required");
        return;
    }

    if (buffer_depth <= 0) {
        buffer_depth = 8;
    }

    const bool forcedSdi = (inputType == 'S' || inputType == 's');
    const bool forcedDv = (inputType == 'D' || inputType == 'd');

    StartWorker(index, [=] {
        std::vector<uint8_t> frame;
        RunCaptureSessions(index, [&] {
            auto board = Board::open(device_id, nullptr);
            auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
            auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);
            auto& rx = board.rx(rx_stream_id);
            const bool forced = forcedSdi || forcedDv;

            // Detection needs a signal. A forced format starts without one: the second link of a
            // dual-stream input (e.g. RX1) only reports a signal once its stream runs.
            if (!forced && !WaitForSignal(index, rx)) return;

            SignalInformation signal_information;
            if (forcedSdi) {
                signal_information = SdiSignalInformation{ video_standard, clock_divisor, video_interface };
            }
            else if (forcedDv) {
                signal_information = DvSignalInformation{ requested_width, requested_height, progressive != 0, framerate, cable_color_space, cable_sampling };
            }
            else { // prefered inputType == 'A'
                signal_information = Application::Helper::detect_information(rx_tech_stream);
            }
            const auto vc = Application::Helper::get_video_characteristics(signal_information);
            const int W = vc.width;
            const int H = vc.height;
            width[index].store(W);
            height[index].store(H);

            // Make info of the signal available as a simple string
            const std::string info = Application::Helper::get_information_string(signal_information, "[Video] ");
            SetVideoInfo(index, info);

            rx_stream.buffer_queue().set_depth(buffer_depth);
            rx_stream.set_buffer_packing(buffer_packing);

            // Preserve the existing merged-frame behavior for fieldMerge != 0.
            // For an interlaced Level-B dual stream, fieldMerge == 0 selects
            // field mode and publishes one bob-deinterlaced frame per field.
            DC_LOG("fieldMerge=" + std::to_string(fieldMerge));
            if (fieldMerge != 0) {
                rx_stream.enable_field_merge();
            }
            const bool useFieldModeBob = ShouldUseFieldModeBob(signal_information, fieldMerge);

            Application::Helper::configure_stream(rx_tech_stream, signal_information);

            if (useFieldModeBob) {
                if (!board.supports_field_mode()) {
                    throw std::runtime_error("The selected DELTACAST board does not support field mode");
                }
                rx_stream.enable_field_mode();
                DC_LOG("field mode bob enabled");
            }

            // The stream stops itself when it goes out of scope (session end or error).
            rx_stream.start();

            frame.resize(size_t(W) * H * 4);
            const int dstPitch = W * 4;
            const auto& matrix = deltacast::MatrixForHeight(H);
            DC_LOG("capturing " + std::to_string(W) + "x" + std::to_string(H));

            while (captureWorkers[index].running.load()) {
                if (!rx.signal_present()) {
                    // Auto mode ends the session and re-detects once the signal is back;
                    // a forced format waits with its stream running.
                    if (!forced || !WaitForSignal(index, rx)) return;
                    SetVideoInfo(index, info);
                    continue;
                }
                // Only auto mode follows the input. A forced format is kept even when
                // detection disagrees (e.g. 1080psf25 and 1080i50 look identical on the wire).
                if (!forced && Application::Helper::detect_information(rx_tech_stream) != signal_information) return;

                auto slot = rx_stream.pop_slot();
                auto [src, totalBytes] = slot->video().buffer();

                if (useFieldModeBob) {
                    const bool evenField = (slot->parity() == Slot::Parity::EVEN);
                    if (!deltacast::UYVY_Field_to_BGRA_Bob(src, totalBytes, frame.data(), dstPitch,
                                                           W, H, evenField, true, matrix)) {
                        DC_LOG("field mode bob: unexpected field buffer size="
                            + std::to_string(totalBytes));
                        continue;
                    }
                }
                else {
                    // Same size rule as the field path: never read past the slot buffer.
                    if (totalBytes < size_t(W) * 2 * H) {
                        DC_LOG("unexpected frame buffer size=" + std::to_string(totalBytes));
                        continue;
                    }
                    deltacast::UYVY_to_BGRA(src, int(totalBytes / H), frame.data(), dstPitch,
                                            W, H, true, matrix);
                }

                PublishFrame(index, frame, W, H, { {0, W} });
            }
        });
    });
}

// Side-by-side stereo from a fixed 1080i50 3G-B dual-stream input: left eye from
// rx_stream_id, right eye always from RX1. Publishes to index 0.
UNITYDLL_EXPORT void StartCaptureStereo(int device_id, int rx_stream_id, int buffer_depth)
{
    if (buffer_depth <= 0) {
        buffer_depth = 8;
    }

    StartWorker(0, [=] {
        std::vector<uint8_t> frame;
        RunCaptureSessions(0, [&] {
            auto board = Board::open(device_id, nullptr);
            auto left_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
            auto right_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(1));
            auto& rx = board.rx(rx_stream_id);

            if (!WaitForSignal(0, rx)) return;

            const SignalInformation signal_information =
                SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
            const auto vc = Application::Helper::get_video_characteristics(signal_information);
            const int W = vc.width;
            const int H = vc.height;
            const auto [outW, outH] = deltacast::stereo_extent(W, H, W, H);

            width[0].store(outW);
            height[0].store(outH);
            SetVideoInfo(0, Application::Helper::get_information_string(signal_information, "[Video] "));

            for (auto* tech_stream : { &left_tech_stream, &right_tech_stream }) {
                auto& stream = Application::Helper::to_base_stream(*tech_stream);
                stream.buffer_queue().set_depth(buffer_depth);
                stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
                stream.enable_field_merge();
                Application::Helper::configure_stream(*tech_stream, signal_information);
            }
            auto& left = Application::Helper::to_base_stream(left_tech_stream);
            auto& right = Application::Helper::to_base_stream(right_tech_stream);
            left.start();
            right.start();

            frame.resize(size_t(outW) * outH * 4);
            const int pitch = outW * 4;
            const auto& matrix = deltacast::MatrixForHeight(H);
            DC_LOG("stereo: capturing " + std::to_string(outW) + "x" + std::to_string(outH));

            while (captureWorkers[0].running.load()) {
                if (!rx.signal_present()) return;

                auto leftSlot = left.pop_slot();
                auto rightSlot = right.pop_slot();
                // simple "bob" deinterlacing of each eye, using SDK-provided parity
                const int keptParity = (leftSlot->parity() == Slot::Parity::EVEN) ? 0 : 1;

                bool complete = true;
                for (int eye = 0; eye < 2 && complete; ++eye) {
                    auto [src, totalBytes] = (eye == 0 ? leftSlot : rightSlot)->video().buffer();
                    if (totalBytes < size_t(W) * 2 * H) {
                        DC_LOG("stereo: unexpected frame buffer size=" + std::to_string(totalBytes));
                        complete = false;
                        break;
                    }
                    // Convert each eye straight into its half of the side-by-side frame.
                    uint8_t* eyeOrigin = frame.data() + size_t(eye) * W * 4;
                    deltacast::UYVY_to_BGRA(src, int(totalBytes / H), eyeOrigin, pitch, W, H, true, matrix);
                    deltacast::BobFillMissingRows(eyeOrigin, W, H, pitch, keptParity);
                }
                if (!complete) continue;

                PublishFrame(0, frame, outW, outH, { {0, W}, {W, W} });
            }
        });
    });
}


    UNITYDLL_EXPORT void StopCapture(int index)
    {
        if (!deltacast::valid_index(index)) return;
        // A recording never outlives its capture; it would only repeat the last frame.
        StopRecording(index);
        captureWorkers[index].stop([index] { ResetStream(index, "Stopped"); });
    }

    UNITYDLL_EXPORT int GetFrame(int index, uint8_t* dst, int maxSize)
    {
        if (!deltacast::valid_index(index)) return 0;
        return publishedFrames[index].copy(dst, maxSize);
    }

} // extern "C"
