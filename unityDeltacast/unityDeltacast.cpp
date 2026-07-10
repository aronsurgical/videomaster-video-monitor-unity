#include "unityDeltacast.h"

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


//static std::atomic<bool> running{ false };
//static std::atomic<bool> running2{ false };
//static std::thread captureThread;
//static std::thread captureThread2;
//
//static std::vector<uint8_t> latestFrame;   // guarded by frameMutex
//static std::vector<uint8_t> latestFrame2;   // guarded by frameMutex
//static std::mutex frameMutex;
//static std::mutex frameMutex2;


static std::atomic<bool> running[4] = { false, false, false, false };
static std::thread captureThread[4];

static std::vector<uint8_t> latestFrame[4];
static std::mutex frameMutex[4];



//static std::vector<uint8_t> bgra;  // scratch BGRA frame
//static std::vector<uint8_t> bgra2;  // scratch BGRA frame

static std::vector<uint8_t> bgra[4];

// simple static message buffer
static std::string message = "Hello Deltacast DLL!\n";
static std::mutex  messageMutex;

//static std::atomic<int> width{ 0 };
//static std::atomic<int> width2{ 0 };
//static std::atomic<int> height{ 0 };
//static std::atomic<int> height2{ 0 };

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

static std::array<std::atomic<unsigned long long>, 4> nativeFrameCounter = {
    std::atomic<unsigned long long>{0},
    std::atomic<unsigned long long>{0},
    std::atomic<unsigned long long>{0},
    std::atomic<unsigned long long>{0}
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


inline uint8_t clamp8(int x) {
    return (uint8_t)(x < 0 ? 0 : x>255 ? 255 : x);
}

// UYVY (U Y0 V Y1) -> BGRA, optional vertical flip
static void UYVY_to_BGRA(const uint8_t* src, int srcPitch, uint8_t* dst, int dstPitch, int W, int H, bool flipY)
{
    for (int y = 0; y < H; ++y) {
        const uint8_t* s = src + y * srcPitch;
        // write to bottom-up row if flipping
        uint8_t* d = dst + (flipY ? (H - 1 - y) * dstPitch : y * dstPitch);

        for (int x = 0; x < W; x += 2) {
            int U = int(s[0]) - 128;
            int Y0 = int(s[1]);
            int V = int(s[2]) - 128;
            int Y1 = int(s[3]);
            s += 4;

            auto emit = [&](int Y) {
                int C = Y - 16; if (C < 0) C = 0;
                int R = clamp8((298 * C + 409 * V + 128) >> 8);
                int G = clamp8((298 * C - 100 * U - 208 * V + 128) >> 8);
                int B = clamp8((298 * C + 516 * U + 128) >> 8);
                *d++ = (uint8_t)B;
                *d++ = (uint8_t)G;
                *d++ = (uint8_t)R;
                *d++ = 255; // A
                };

            emit(Y0);
            emit(Y1);
        }
    }
}

// Convert one packed UYVY field to a full-height BGRA frame using bob
// deinterlacing. Field lines are copied to their natural parity and the missing
// lines are interpolated from the nearest captured lines. The SDK returns field
// lines contiguously; totalBytes may describe either the field payload or a
// full-frame-sized slot allocation, so derive padding only for a half-frame
// payload and otherwise use the natural UYVY row size.
static bool UYVY_Field_to_BGRA_Bob(const uint8_t* src,
                                   size_t totalBytes,
                                   uint8_t* dst,
                                   int dstPitch,
                                   int W,
                                   int H,
                                   bool evenField,
                                   bool flipY)
{
    if (!src || !dst || W <= 0 || H <= 0 || (W & 1) != 0) return false;

    // Field 1/odd carries display rows 0,2,4...; field 2/even carries
    // display rows 1,3,5... (zero-based row coordinates).
    const int sourceParity = evenField ? 1 : 0;
    const int fieldRows = (H - sourceParity + 1) / 2;
    if (fieldRows <= 0) return false;

    const size_t rowBytes = size_t(W) * 2; // UYVY 4:2:2 8-bit
    const size_t minimumFieldBytes = rowBytes * size_t(fieldRows);
    if (totalBytes < minimumFieldBytes) return false;

    size_t srcPitch = rowBytes;
    const size_t minimumFrameBytes = rowBytes * size_t(H);
    if (totalBytes < minimumFrameBytes && totalBytes % size_t(fieldRows) == 0) {
        const size_t candidatePitch = totalBytes / size_t(fieldRows);
        if (candidatePitch >= rowBytes) srcPitch = candidatePitch;
    }

    if ((size_t(fieldRows - 1) * srcPitch) + rowBytes > totalBytes) return false;

    // Convert every real field line into its full-frame row.
    for (int fieldY = 0; fieldY < fieldRows; ++fieldY) {
        const int sourceFrameY = fieldY * 2 + sourceParity;
        const int outputY = flipY ? (H - 1 - sourceFrameY) : sourceFrameY;

        UYVY_to_BGRA(
            src + size_t(fieldY) * srcPitch,
            int(srcPitch),
            dst + size_t(outputY) * dstPitch,
            dstPitch,
            W,
            1,
            false);
    }

    // Vertical flipping reverses line parity when the output height is even.
    const int outputFieldParity = flipY ? ((H - 1 - sourceParity) & 1) : sourceParity;
    const int rowBytesBGRA = W * 4;

    for (int y = 0; y < H; ++y) {
        if ((y & 1) == outputFieldParity) continue;

        int upY = y - 1;
        int downY = y + 1;
        if (upY < 0) upY = downY;
        if (downY >= H) downY = upY;

        uint8_t* out = dst + size_t(y) * dstPitch;
        const uint8_t* up = dst + size_t(upY) * dstPitch;
        const uint8_t* down = dst + size_t(downY) * dstPitch;

        if (upY == downY) {
            std::memcpy(out, up, rowBytesBGRA);
        }
        else {
            for (int i = 0; i < rowBytesBGRA; ++i) {
                out[i] = uint8_t((int(up[i]) + int(down[i])) >> 1);
            }
        }
    }

    return true;
}

//using TechStream = std::variant<Deltacast::Wrapper::SdiStream, Deltacast::Wrapper::DvStream>;

static std::vector<uint8_t> sbsBGRA;  // combined (left|right) output

// copy left & right BGRA images into a single side-by-side BGRA
static void PackSideBySide_BGRA(const uint8_t* left, int LW, int LH, int LPitch,
    const uint8_t* right, int RW, int RH, int RPitch,
    uint8_t* dst, int outW, int outH)
{
    const int outPitch = outW * 4;
    const int copyLeftBytes = LW * 4;
    const int copyRightBytes = RW * 4;
    const int rightOffset = LW * 4;

    for (int y = 0; y < outH; ++y) {
        uint8_t* drow = dst + y * outPitch;

        if (y < LH) {
            std::memcpy(drow, left + y * LPitch, copyLeftBytes);
        }
        else {
            std::memset(drow, 0, copyLeftBytes);
        }

        if (y < RH) {
            std::memcpy(drow + rightOffset, right + y * RPitch, copyRightBytes);
        }
        else {
            std::memset(drow + rightOffset, 0, copyRightBytes);
        }
    }
}

// copy left & right BGRA images into a single side-by-side BGRA
// with simple "bob" deinterlacing, using SDK-provided parity
static void PackSideBySide_BGRA_deinterlaced(const uint8_t* left, int LW, int LH, int LPitch,
    const uint8_t* right, int RW, int RH, int RPitch,
    uint8_t* dst, int outW, int outH,
    bool topFieldFirst)  
{
    const int outPitch = outW * 4;
    const int leftBytesRow = LW * 4;
    const int rightBytesRow = RW * 4;
    const int rightOffset = LW * 4; // bytes to the start of the right image

    for (int y = 0; y < outH; ++y) {
        uint8_t* drow = dst + y * outPitch;

        // ---------- LEFT ----------
        if (y < LH) {
            const uint8_t* crow = left + y * LPitch;

            bool isFieldLine = ((y & 1) == 0);
            if (!topFieldFirst) isFieldLine = !isFieldLine;  // flip if bottom-field-first

            if (isFieldLine) {
                // copy original field line
                std::memcpy(drow, crow, leftBytesRow);
            }
            else {
                // interpolate odd line
                const int upY = (y > 0) ? (y - 1) : y;
                const int dnY = (y + 1 < LH) ? (y + 1) : y;
                const uint8_t* up = left + upY * LPitch;
                const uint8_t* dn = left + dnY * LPitch;
                for (int i = 0; i < leftBytesRow; ++i) {
                    drow[i] = uint8_t((int(up[i]) + int(dn[i])) >> 1);
                }
            }
        }
        else {
            std::memset(drow, 0, leftBytesRow);
        }

        // ---------- RIGHT ----------
        uint8_t* drowR = drow + rightOffset;
        if (y < RH) {
            const uint8_t* crow = right + y * RPitch;

            bool isFieldLine = ((y & 1) == 0);
            if (!topFieldFirst) isFieldLine = !isFieldLine;

            if (isFieldLine) {
                std::memcpy(drowR, crow, rightBytesRow);
            }
            else {
                const int upY = (y > 0) ? (y - 1) : y;
                const int dnY = (y + 1 < RH) ? (y + 1) : y;
                const uint8_t* up = right + upY * RPitch;
                const uint8_t* dn = right + dnY * RPitch;
                for (int i = 0; i < rightBytesRow; ++i) {
                    drowR[i] = uint8_t((int(up[i]) + int(dn[i])) >> 1);
                }
            }
        }
        else {
            std::memset(drowR, 0, rightBytesRow);
        }
    }
}



static void logHelper(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(messageMutex);
    if (message.size() < 2000) {
        message += msg + "\n";
    }
}

//void StartCaptureAuto(int device_id, int rx_stream_id, int buffer_depth)
//{
//    bool expected = false;
//    if (!running[0].compare_exchange_strong(expected, true)) return; // already running
//
//    if (buffer_depth <= 0) {
//        buffer_depth = 8;
//    }
//
//    captureThread[0] = std::thread([device_id, rx_stream_id, buffer_depth]() {
//        bool started = false;
//        try {
//            auto board = Board::open(device_id, nullptr);
//
//            // open RX tech stream and get base stream
//            auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
//            auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);
//
//            // 1) Wait until a signal is present on the connector
//            if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[0])) {
//                throw std::runtime_error("No input detected on the requested RX");
//            }
//
//            // 2) Detect signal information (format, framerate, etc.)
//            auto signal_information = Application::Helper::detect_information(rx_tech_stream);
//            //signal_information = SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
//            //signal_information = DvSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
//            auto vc = Application::Helper::get_video_characteristics(signal_information);
//            int W = vc.width;
//            int H = vc.height;
//            width[0].store(W);
//            height[0].store(H);
//
//
//            // 3) Queue depth & packing
//            rx_stream.buffer_queue().set_depth(buffer_depth);
//            rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
//
//            //field merge necesary for example in stereo dual stream
//            //rx_stream.enable_field_merge();
//
//            // 4) Configure stream to match the detected signal
//            Application::Helper::configure_stream(rx_tech_stream, signal_information);
//
//
//
//            // 5) Start the stream
//            rx_stream.start();
//
//            started = true;
//
//            bgra[0].resize(size_t(width[0]) * height[0] * 4);
//
//            DC_LOG("width=" + std::to_string(width[0]));
//            DC_LOG("height=" + std::to_string(height[0]));
//
//            while (running[0].load()) {
//                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[0])) {
//                    continue;
//                }
//
//                // If signal changes mid-run, reconfigure
//                auto cur = Application::Helper::detect_information(rx_tech_stream);
//                if (cur != signal_information) {
//                    if (started) { 
//                        rx_stream.stop();
//                        started = false;
//                    }
//                    signal_information = cur;
//                    vc = Application::Helper::get_video_characteristics(signal_information);
//                    W = vc.width;
//                    H = vc.height;
//                    width[0].store(W); height[0].store(H);
//                    bgra[0].resize(size_t(W) * H * 4);
//
//                    rx_stream.buffer_queue().set_depth(buffer_depth);
//                    rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
//                    Application::Helper::configure_stream(rx_tech_stream, signal_information);
//                    rx_stream.start();
//                    started = true;
//                    continue;
//                }
//
//                auto slot = rx_stream.pop_slot();
//                auto [src, totalBytes] = slot->video().buffer();
//
//                // derive source pitch (handles driver alignment)
//                if (height[0] <= 0) {
//                    continue;
//                }
//                int srcPitch = int(totalBytes / height[0]);   // sanity: if (totalBytes % H) -> alignment mismatch
//                int dstPitch = width[0] * 4;
//
//                UYVY_to_BGRA(src, srcPitch, bgra[0].data(), dstPitch, width[0], height[0], true);
//
//                {
//                    std::lock_guard<std::mutex> lock(frameMutex[0]);
//                    latestFrame[0] = bgra[0];  // hand BGRA to Unity
//                }
//                //log("running");
//            }
//            if (started) {
//                rx_stream.stop();
//                started = false;
//            }
//            DC_LOG("finished");
//        }
//        catch (const ApiException& e) {
//            DC_LOG(std::string("ApiException: ") + e.what());
//        }
//        catch (const std::exception& e) {
//            DC_LOG(std::string("std::exception: ") + e.what());
//        }
//        });
//}

static const uint8_t DIGITS_3x5[10][5] = {
    {0b111, 0b101, 0b101, 0b101, 0b111}, // 0
    {0b010, 0b110, 0b010, 0b010, 0b111}, // 1
    {0b111, 0b001, 0b111, 0b100, 0b111}, // 2
    {0b111, 0b001, 0b111, 0b001, 0b111}, // 3
    {0b101, 0b101, 0b111, 0b001, 0b001}, // 4
    {0b111, 0b100, 0b111, 0b001, 0b111}, // 5
    {0b111, 0b100, 0b111, 0b101, 0b111}, // 6
    {0b111, 0b001, 0b001, 0b001, 0b001}, // 7
    {0b111, 0b101, 0b111, 0b101, 0b111}, // 8
    {0b111, 0b101, 0b111, 0b001, 0b111}  // 9
};

static void SetPixelBGRA(uint8_t* img, int W, int H, int pitch, int x, int y,
    uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255)
{
    if (x < 0 || y < 0 || x >= W || y >= H) return;

    uint8_t* p = img + y * pitch + x * 4;
    p[0] = b;
    p[1] = g;
    p[2] = r;
    p[3] = a;
}

static void FillRectBGRA(uint8_t* img, int W, int H, int pitch,
    int x0, int y0, int rw, int rh,
    uint8_t b, uint8_t g, uint8_t r, uint8_t a = 255)
{
    for (int y = y0; y < y0 + rh; ++y) {
        for (int x = x0; x < x0 + rw; ++x) {
            SetPixelBGRA(img, W, H, pitch, x, y, b, g, r, a);
        }
    }
}

static void DrawDigit3x5BGRA(uint8_t* img, int W, int H, int pitch,
    int digit, int x0, int y0, int scale,
    uint8_t b, uint8_t g, uint8_t r,
    bool flipTextY)
{
    if (digit < 0 || digit > 9) return;

    for (int row = 0; row < 5; ++row) {
        int srcRow = flipTextY ? (4 - row) : row;
        uint8_t bits = DIGITS_3x5[digit][srcRow];

        for (int col = 0; col < 3; ++col) {
            bool on = (bits & (1 << (2 - col))) != 0;
            if (!on) continue;

            for (int sy = 0; sy < scale; ++sy) {
                for (int sx = 0; sx < scale; ++sx) {
                    SetPixelBGRA(
                        img, W, H, pitch,
                        x0 + col * scale + sx,
                        y0 + row * scale + sy,
                        b, g, r, 255
                    );
                }
            }
        }
    }
}

static void BurnFrameNumberBGRA(uint8_t* img,
    int imageW,
    int imageH,
    int pitch,
    unsigned long long frameNo,
    int regionX,
    int regionW,
    bool bufferIsVerticallyFlipped)
{
    if (!img || imageW <= 0 || imageH <= 0 || pitch <= 0 || regionW <= 0) {
        return;
    }

    const int scale = 2;
    const int gap = scale;
    const int margin = 4;

    std::string text = std::to_string(frameNo);

    int digitW = 3 * scale;
    int digitH = 5 * scale;
    int textW = int(text.size()) * digitW + int(text.size() - 1) * gap;
    int textH = digitH;

    int x = regionX + regionW - textW - margin;

    // If the buffer is vertically flipped before Unity displays it,
    // native bottom becomes visible top.
    int y = bufferIsVerticallyFlipped
        ? imageH - margin - textH
        : margin;

    FillRectBGRA(img, imageW, imageH, pitch,
        x - 2, y - 2,
        textW + 4, textH + 4,
        0, 0, 0, 255);

    int cx = x;
    for (char c : text) {
        if (c >= '0' && c <= '9') {
            DrawDigit3x5BGRA(
                img, imageW, imageH, pitch,
                c - '0',
                cx, y,
                scale,
                255, 255, 255,
                bufferIsVerticallyFlipped
            );
        }

        cx += digitW + gap;
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
};

static RecorderCtx recorders[4];

// Assemble the ffmpeg argument string, identical to the known-good Unity command.
static std::string BuildFfmpegCommand(const RecorderCtx& r)
{
    const std::string defaultEncoder =
        "-c:v h264_nvenc -preset p1 -rc vbr -cq 22 -pix_fmt yuv420p -g "
        + std::to_string(r.fps) + " -rgb_mode yuv420";
    const std::string& encoder = r.encoderArgs.empty() ? defaultEncoder : r.encoderArgs;

    std::ostringstream cmd;
    cmd << "\"" << r.ffmpegExe << "\""
        << " -y -f rawvideo -pixel_format bgra"
        << " -video_size " << r.width << "x" << r.height
        << " -framerate " << r.fps
        << " -i -";
    if (r.vflip) cmd << " -vf vflip";
    cmd << " " << encoder
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
    hStdInWr = nullptr;
    ZeroMemory(&pi, sizeof(pi));

    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE hStdInRd = nullptr;
    if (!CreatePipe(&hStdInRd, &hStdInWr, &sa, 0)) {
        DC_LOG("rec: CreatePipe failed err=" + std::to_string(GetLastError()));
        return false;
    }
    // The write end stays in the parent and must NOT be inherited by the child.
    SetHandleInformation(hStdInWr, HANDLE_FLAG_INHERIT, 0);

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
    unsigned long long lastCapturedSeen = 0;  // last capture-frame counter we copied
    unsigned long long nextIdx = 0;
    bool resolutionChanged = false;

    while (r.recording.load(std::memory_order_relaxed)) {
        const auto target = t0 + std::chrono::nanoseconds(
            (long long)(double(nextIdx) * interval * 1e9));
        std::this_thread::sleep_until(target);

        // Only copy when capture produced a new frame; otherwise reuse `last` (gap-fill) without
        // taking the lock. nativeFrameCounter is updated immediately after each publish, so its
        // release/acquire ordering makes it a cheap, lock-free "new frame available" signal.
        const unsigned long long captured = nativeFrameCounter[index].load(std::memory_order_acquire);
        if (captured != lastCapturedSeen) {
            std::lock_guard<std::mutex> lk(frameMutex[index]);
            const size_t avail = latestFrame[index].size();
            if (avail == frameBytes) {
                last.assign(latestFrame[index].begin(), latestFrame[index].end());
                haveLast = true;
                lastCapturedSeen = captured;
            }
            else if (avail != 0) {
                resolutionChanged = true;  // signal change mid-recording -> stop cleanly
            }
        }

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

    UNITYDLL_EXPORT void InitLibrary() {
        DC_LOG("Library initialized!");
    }

    UNITYDLL_EXPORT int AddInts(int a, int b) {
        return a + b;
    }

    UNITYDLL_EXPORT const char* GetMessage() {
        static std::string last;
        std::lock_guard<std::mutex> lk(messageMutex);
        last = message;
        message.clear();
        return last.c_str(); // 'last' persists between calls
    }

    UNITYDLL_EXPORT int GetWidth(int index) {
        return width[index].load();
    }

    UNITYDLL_EXPORT int GetHeight(int index) {
        return height[index].load();
    }

    UNITYDLL_EXPORT void SetBurnInFrameNumber(int index, int enabled)
    {
        if (index < 0 || index >= 4) return;

        burnInFrameNumber[index].store(enabled != 0, std::memory_order_relaxed);
    }

    UNITYDLL_EXPORT unsigned long long GetNativeFrameCounter(int index)
    {
        if (index < 0 || index >= 4) return 0;

        return nativeFrameCounter[index].load(std::memory_order_relaxed);
    }

    UNITYDLL_EXPORT void GetSignalAndVideoInfo(int index, char* buffer, int bufferSize) {
        if (index < 0 || index >= 4 || !buffer || bufferSize <= 0)
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
        if (index < 0 || index >= 4) return 0;
        if (!outputPattern || !*outputPattern) return 0;
        if (fps <= 0) fps = 30;
        if (segmentSeconds <= 0) segmentSeconds = 60;

        RecorderCtx& r = recorders[index];

        bool expected = false;
        if (!r.recording.compare_exchange_strong(expected, true)) {
            return 0; // already recording
        }

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
    }

    UNITYDLL_EXPORT void StopRecording(int index)
    {
        if (index < 0 || index >= 4) return;
        RecorderCtx& r = recorders[index];
        r.recording.store(false, std::memory_order_relaxed);
        if (r.thread.joinable()) r.thread.join();
    }

    UNITYDLL_EXPORT int IsRecording(int index)
    {
        if (index < 0 || index >= 4) return 0;
        return recorders[index].recording.load(std::memory_order_relaxed) ? 1 : 0;
    }

    UNITYDLL_EXPORT unsigned long long GetRecordedFrameNumber(int index)
    {
        if (index < 0 || index >= 4) return 0;
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
    if (index < 0 || index >= 4) {
        return;
    }

    bool expected = false;
    if (!running[index].compare_exchange_strong(expected, true)) return; // already running

    nativeFrameCounter[index].store(0, std::memory_order_relaxed);

    if (buffer_depth <= 0) {
        buffer_depth = 8;
    }

        captureThread[index] = std::thread([index, device_id, rx_stream_id, buffer_depth, inputType, requested_width, requested_height, progressive, framerate, cable_color_space, cable_sampling, video_standard, clock_divisor, video_interface, fieldMerge, buffer_packing]() {
            bool started = false;
            try {
                auto board = Board::open(device_id, nullptr);

                // open RX tech stream and get base stream
                auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
                auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);

                // 1) Wait until a signal is present on the connector
                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[index])) {
                    //throw std::runtime_error("No input detected on the requested RX");
                }

                // 2) Detect signal information (format, framerate, etc.)
                

                SignalInformation signal_information;
                if (inputType == 'S' || inputType == 's'){
                    signal_information = SdiSignalInformation{ video_standard, clock_divisor, video_interface };
                }
                else if(inputType == 'D' || inputType == 'd'){
                    signal_information = DvSignalInformation{ requested_width, requested_height, progressive != 0, framerate, cable_color_space, cable_sampling };

                }
                else {// prefered inputType == 'A'
                    signal_information = Application::Helper::detect_information(rx_tech_stream);
                }
                auto vc = Application::Helper::get_video_characteristics(signal_information);
                int W = vc.width;
                int H = vc.height;
                width[index].store(W);
                height[index].store(H);

                // Make info of the signal available as a simple string
                SetVideoInfo(index, Application::Helper::get_information_string(signal_information, "[Video] "));

                // 3) Queue depth & packing
                rx_stream.buffer_queue().set_depth(buffer_depth);
                rx_stream.set_buffer_packing(buffer_packing);

                // Preserve the existing merged-frame behavior for fieldMerge != 0.
                // For an interlaced Level-B dual stream, fieldMerge == 0 selects
                // field mode and publishes one bob-deinterlaced frame per field.
                DC_LOG("fieldMerge=" + std::to_string(fieldMerge));
                if (fieldMerge != 0) {
                    rx_stream.enable_field_merge();
                }

                bool useFieldModeBob = ShouldUseFieldModeBob(signal_information, fieldMerge);

                // 4) Configure stream to match the detected signal
                Application::Helper::configure_stream(rx_tech_stream, signal_information);

                if (useFieldModeBob) {
                    if (!board.supports_field_mode()) {
                        throw std::runtime_error("The selected DELTACAST board does not support field mode");
                    }
                    rx_stream.enable_field_mode();
                    DC_LOG("field mode bob enabled");
                }



                // 5) Start the stream
                rx_stream.start();

                started = true;

                bgra[index].resize(size_t(width[index]) * height[index] * 4);

                DC_LOG("width=" + std::to_string(width[index]));
                DC_LOG("height=" + std::to_string(height[index]));

                while (running[index].load()) {
                    if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[index])) {
                        continue;
                    }
                    // If signal changes mid-run, reconfigure
                    auto cur = Application::Helper::detect_information(rx_tech_stream);
                    if (cur != signal_information) {
                        if (started) { 
                            rx_stream.stop();
                            started = false; 
                        }
                        signal_information = cur;
                        SetVideoInfo(index, Application::Helper::get_information_string(signal_information, "[Video] "));
                        vc = Application::Helper::get_video_characteristics(signal_information);
                        W = vc.width;
                        H = vc.height;
                        width[index].store(W);
                        height[index].store(H);
                        bgra[index].resize(size_t(W) * H * 4);

                        rx_stream.buffer_queue().set_depth(buffer_depth);
                        rx_stream.set_buffer_packing(buffer_packing);

                        const bool newUseFieldModeBob = ShouldUseFieldModeBob(signal_information, fieldMerge);
                        if (useFieldModeBob && !newUseFieldModeBob) {
                            rx_stream.disable_field_mode();
                        }

                        Application::Helper::configure_stream(rx_tech_stream, signal_information);

                        if (!useFieldModeBob && newUseFieldModeBob) {
                            if (!board.supports_field_mode()) {
                                throw std::runtime_error("The selected DELTACAST board does not support field mode");
                            }
                            rx_stream.enable_field_mode();
                        }
                        useFieldModeBob = newUseFieldModeBob;

                        rx_stream.start();
                        started = true;
                        continue;
                    }
                    auto slot = rx_stream.pop_slot();
                    auto [src, totalBytes] = slot->video().buffer();
                    if (height[index] <= 0) {
                        continue;
                    }
                    int dstPitch = width[index] * 4;

                    if (useFieldModeBob) {
                        const bool evenField = (slot->parity() == Slot::Parity::EVEN);
                        if (!UYVY_Field_to_BGRA_Bob(
                                src,
                                totalBytes,
                                bgra[index].data(),
                                dstPitch,
                                width[index],
                                height[index],
                                evenField,
                                true)) {
                            DC_LOG("field mode bob: unexpected field buffer size="
                                + std::to_string(totalBytes));
                            continue;
                        }
                    }
                    else {
                        // Derive source pitch for the legacy full-frame path.
                        int srcPitch = int(totalBytes / height[index]);
                        UYVY_to_BGRA(src, srcPitch, bgra[index].data(), dstPitch,
                            width[index], height[index], true);
                    }

                    const unsigned long long frameNo =
                        nativeFrameCounter[index].load(std::memory_order_relaxed) + 1;

                    if (burnInFrameNumber[index].load(std::memory_order_relaxed)) {
                        BurnFrameNumberBGRA(
                            bgra[index].data(),
                            width[index],
                            height[index],
                            dstPitch,
                            frameNo,
                            0,
                            width[index],
                            true
                        );
                    }
                    {
                        std::lock_guard<std::mutex> lock(frameMutex[index]);
                        latestFrame[index] = bgra[index];  // hand BGRA to Unity
                    }

                    // Publish the counter only after latestFrame is complete. The recorder's
                    // acquire load then cannot associate a new counter with the old pixels.
                    nativeFrameCounter[index].store(frameNo, std::memory_order_release);

                    //log("running");
                }
                if (started) {
                    rx_stream.stop();
                    started = false;
                }
                DC_LOG("finished");
            }
            catch (const ApiException& e) {
                DC_LOG(std::string("ApiException: ") + e.what());
            }
            catch (const std::exception& e) {
                DC_LOG(std::string("std::exception: ") + e.what());
            }
            });
    }

    ////AW: HACK!!! remember to always use both methods
    //UNITYDLL_EXPORT void StartCapture2(int device_id, int rx_stream_id, int buffer_depth, char inputType, unsigned int requested_width, unsigned int requested_height, unsigned int progressive, unsigned int framerate, VHD_DV_CS cable_color_space, VHD_DV_SAMPLING cable_sampling, VHD_VIDEOSTANDARD video_standard, VHD_CLOCKDIVISOR clock_divisor, VHD_INTERFACE video_interface, unsigned int fieldMerge, VHD_BUFFERPACKING buffer_packing)
    //{
    //    bool expected = false;
    //    if (!running2.compare_exchange_strong(expected, true)) return; // already running

    //    if (buffer_depth <= 0) {
    //        buffer_depth = 8;
    //    }

    //    captureThread2 = std::thread([device_id, rx_stream_id, buffer_depth, inputType, requested_width, requested_height, progressive, framerate, cable_color_space, cable_sampling, video_standard, clock_divisor, video_interface, fieldMerge, buffer_packing]() {
    //        bool started = false;
    //        try {
    //            auto board = Board::open(device_id, nullptr);

    //            // open RX tech stream and get base stream
    //            auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
    //            auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);

    //            // 1) Wait until a signal is present on the connector
    //            if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running2)) {
    //                throw std::runtime_error("No input detected on the requested RX");
    //            }

    //            // 2) Detect signal information (format, framerate, etc.)


    //            SignalInformation signal_information;
    //            if (inputType == 'S' || inputType == 's') {
    //                signal_information = SdiSignalInformation{ video_standard, clock_divisor, video_interface };
    //            }
    //            else if (inputType == 'D' || inputType == 'd') {
    //                signal_information = DvSignalInformation{ requested_width, requested_height, progressive != 0, framerate, cable_color_space, cable_sampling };

    //            }
    //            else {// prefered inputType == 'A'
    //                signal_information = Application::Helper::detect_information(rx_tech_stream);
    //            }

    //            auto vc = Application::Helper::get_video_characteristics(signal_information);
    //            int W = vc.width;
    //            int H = vc.height;
    //            width2.store(W);
    //            height2.store(H);


    //            // 3) Queue depth & packing
    //            rx_stream.buffer_queue().set_depth(buffer_depth);
    //            rx_stream.set_buffer_packing(buffer_packing);

    //            //field merge necesary for example in stereo dual stream
    //            DC_LOG("fieldMerge=" + std::to_string(fieldMerge));
    //            if (fieldMerge != 0) {
    //                rx_stream.enable_field_merge();
    //            }

    //            // 4) Configure stream to match the detected signal
    //            Application::Helper::configure_stream(rx_tech_stream, signal_information);



    //            // 5) Start the stream
    //            rx_stream.start();

    //            started = true;

    //            bgra2.resize(size_t(width2) * height2 * 4);

    //            DC_LOG("width=" + std::to_string(width2));
    //            DC_LOG("height=" + std::to_string(height2));

    //            while (running2.load()) {
    //                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running2)) {
    //                    continue;
    //                }

    //                // If signal changes mid-run, reconfigure
    //                auto cur = Application::Helper::detect_information(rx_tech_stream);
    //                if (cur != signal_information) {
    //                    if (started) { rx_stream.stop(); started = false; }
    //                    signal_information = cur;
    //                    vc = Application::Helper::get_video_characteristics(signal_information);
    //                    W = vc.width; H = vc.height;
    //                    width2.store(W); height2.store(H);
    //                    bgra2.resize(size_t(W) * H * 4);

    //                    rx_stream.buffer_queue().set_depth(buffer_depth);
    //                    rx_stream.set_buffer_packing(buffer_packing);
    //                    Application::Helper::configure_stream(rx_tech_stream, signal_information);
    //                    rx_stream.start();
    //                    started = true;
    //                    continue;
    //                }

    //                auto slot = rx_stream.pop_slot();
    //                auto [src, totalBytes] = slot->video().buffer();

    //                // derive source pitch (handles driver alignment)
    //                if (height2 <= 0) {
    //                    continue;
    //                }
    //                int srcPitch = int(totalBytes / height2);   // sanity: if (totalBytes % H) -> alignment mismatch
    //                int dstPitch = width2 * 4;

    //                UYVY_to_BGRA(src, srcPitch, bgra2.data(), dstPitch, width2, height2, true);

    //                {
    //                    std::lock_guard<std::mutex> lock(frameMutex2);
    //                    latestFrame2 = bgra2;  // hand BGRA to Unity
    //                }
    //                //log("running");
    //            }
    //            if (started) {
    //                rx_stream.stop();
    //                started = false;
    //            }
    //            DC_LOG("finished");
    //        }
    //        catch (const ApiException& e) {
    //            DC_LOG(std::string("ApiException: ") + e.what());
    //        }
    //        catch (const std::exception& e) {
    //            DC_LOG(std::string("std::exception: ") + e.what());
    //        }
    //        });
    //}

UNITYDLL_EXPORT void StartCaptureStereo(int device_id, int rx_stream_id, int buffer_depth)
{
    bool expected = false;
    if (!running[0].compare_exchange_strong(expected, true)) return; // already running

    nativeFrameCounter[0].store(0, std::memory_order_relaxed);

    if (buffer_depth <= 0) {
        buffer_depth = 8;
    }

        captureThread[0] = std::thread([device_id, rx_stream_id, buffer_depth]() {
            bool started = false;
            try {
                auto board = Board::open(device_id, nullptr);
                DC_LOG("0");
                // open RX tech stream and get base stream
                auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
                auto rx_tech_stream2 = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(1));
                auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);
                auto& rx_stream2 = Application::Helper::to_base_stream(rx_tech_stream2);
                DC_LOG("1");
                // 1) Wait until a signal is present on the connector
                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[0])) {
                    throw std::runtime_error("No input detected on the requested RX");
                }
                DC_LOG("2");
                // 2) Detect signal information (format, framerate, etc.)
                SignalInformation signal_information;// = Application::Helper::detect_information(rx_tech_stream);
                SignalInformation signal_information2;// = Application::Helper::detect_information(rx_tech_stream2);
                DC_LOG("2.1");
                signal_information = SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
                signal_information2 = SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
                
                DC_LOG("2.2");
                auto vc = Application::Helper::get_video_characteristics(signal_information);
                auto vc2 = Application::Helper::get_video_characteristics(signal_information2);
                DC_LOG("2.3");
                // after vc / vc2 have been computed:
                int W = vc.width, H = vc.height;
                int W2 = vc2.width, H2 = vc2.height;

                int outW = W + W2;
                int outH = (H > H2) ? H : H2;

                width[0].store(outW);
                height[0].store(outH);

                // Make info of the signal available as a simple string (stereo publishes to index 0)
                SetVideoInfo(0, Application::Helper::get_information_string(signal_information, "[Video] "));

                // allocate working & output buffers
                bgra[0].resize(size_t(W) * H * 4);
                bgra[1].resize(size_t(W2) * H2 * 4);
                sbsBGRA.resize(size_t(outW) * outH * 4);
                DC_LOG("stereo: L=" + std::to_string(W) + "x" + std::to_string(H)
                    + " R=" + std::to_string(W2) + "x" + std::to_string(H2)
                    + " out=" + std::to_string(outW) + "x" + std::to_string(outH));

                DC_LOG("3");
                // 3) Queue depth & packing
                rx_stream.buffer_queue().set_depth(buffer_depth);
                rx_stream2.buffer_queue().set_depth(buffer_depth);
                rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
                rx_stream2.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);

                DC_LOG("?");
                //????
                rx_stream.enable_field_merge();
                rx_stream2.enable_field_merge();
                //rx_stream.set_low_latency_mode(VHD_LLM_DATA_BLOCK);
                //rx_stream2.set_low_latency_mode(VHD_LLM_DATA_BLOCK);

                DC_LOG("4");
                // 4) Configure stream to match the detected signal
                Application::Helper::configure_stream(rx_tech_stream, signal_information);
                Application::Helper::configure_stream(rx_tech_stream2, signal_information2);


                DC_LOG("5");
                // 5) Start the stream
                rx_stream.start();
                rx_stream2.start();

                started = true;

                while (running[0].load()) {
                    if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running[0])) {
                        continue;
                    }
                    DC_LOG("6");
                    // If signal changes mid-run, reconfigure
                    auto cur = Application::Helper::detect_information(rx_tech_stream);
                    if (cur != signal_information) {
                        if (started) {
                            rx_stream.stop();
                            started = false; 
                        }
                        DC_LOG("6");
                        signal_information = cur;
                        SetVideoInfo(0, Application::Helper::get_information_string(signal_information, "[Video] "));
                        vc = Application::Helper::get_video_characteristics(signal_information);
                        W = vc.width; H = vc.height;
                        width[0].store(W);
                        height[0].store(H);
                        bgra[0].resize(size_t(W) * H * 4);

                        rx_stream.buffer_queue().set_depth(buffer_depth);
                        rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
                        Application::Helper::configure_stream(rx_tech_stream, signal_information);
                        rx_stream.start();
                        started = true;
                        continue;
                    }
                    DC_LOG("8");
                    auto slot = rx_stream.pop_slot();
                    auto slot2 = rx_stream2.pop_slot();
                    auto [src, totalBytes] = slot->video().buffer();
                    auto [src2, totalBytes2] = slot2->video().buffer();

                    int srcPitch = (H > 0) ? int(totalBytes / H) : 0;
                    int srcPitch2 = (H2 > 0) ? int(totalBytes2 / H2) : 0;

                    int dstPitch = W * 4;
                    int dstPitch2 = W2 * 4;
                    DC_LOG("9");
                    UYVY_to_BGRA(src, srcPitch, bgra[0].data(), dstPitch, W, H, true);
                    UYVY_to_BGRA(src2, srcPitch2, bgra[1].data(), dstPitch2, W2, H2, true);

                    bool topFieldFirst = (slot->parity() == Slot::Parity::EVEN);
                    // pack into one wide texture
                    PackSideBySide_BGRA_deinterlaced(bgra[0].data(), W, H, dstPitch,
                        bgra[1].data(), W2, H2, dstPitch2,
                        sbsBGRA.data(), outW, outH, topFieldFirst);

                    const unsigned long long frameNo =
                        nativeFrameCounter[0].fetch_add(1, std::memory_order_relaxed) + 1;

                    if (burnInFrameNumber[0].load(std::memory_order_relaxed)) {
                        const int outPitch = outW * 4;

                        // Burn into upper-right corner of the left eye.
                        BurnFrameNumberBGRA(
                            sbsBGRA.data(),
                            outW,
                            outH,
                            outPitch,
                            frameNo,
                            0,
                            W,
                            true
                        );

                        // Burn the same number into upper-right corner of the right eye.
                        BurnFrameNumberBGRA(
                            sbsBGRA.data(),
                            outW,
                            outH,
                            outPitch,
                            frameNo,
                            W,
                            W2,
                            true
                        );
                    }
                    // publish the single combined frame
                    {
                        std::lock_guard<std::mutex> lock(frameMutex[0]);
                        latestFrame[0] = sbsBGRA;
                    }


                    //log("running");
                    DC_LOG("10");
                }
                DC_LOG("11");
                if (started) {
                    rx_stream.stop();
                    rx_stream2.stop();
                    started = false;
                }
                DC_LOG("finished");
            }
            catch (const ApiException& e) {
                DC_LOG(std::string("ApiException: ") + e.what());
            }
            catch (const std::exception& e) {
                DC_LOG(std::string("std::exception: ") + e.what());
            }
            });
    }


    UNITYDLL_EXPORT void StopCapture(int index)
    {
        running[index].store(false);
        if (captureThread[index].joinable()) {
            captureThread[index].join();
        }
        if (index >= 0 && index < 4) {
            std::lock_guard<std::mutex> lk(videoInfoMutex[index]);
            videoInfo[index] = "Stopped";
        }
    }

    //UNITYDLL_EXPORT void StopCapture2()
    //{
    //    running2.store(false);
    //    if (captureThread2.joinable()) {
    //        captureThread2.join();
    //    }
    //}

    UNITYDLL_EXPORT int GetFrame(int index, uint8_t* dst, int maxSize)
    {
        std::lock_guard<std::mutex> lk(frameMutex[index]);
        int n = std::min<int>(maxSize, latestFrame[index].size());
        if (n > 0) {
            std::memcpy(dst, latestFrame[index].data(), n);
        }
        return n;
    }

    //UNITYDLL_EXPORT int GetFrame2(uint8_t* dst, int maxSize)
    //{
    //    std::lock_guard<std::mutex> lk(frameMutex2);
    //    int n = std::min<int>(maxSize, latestFrame2.size());
    //    if (n > 0) {
    //        std::memcpy(dst, latestFrame2.data(), n);
    //    }
    //    return n;
    //}



} // extern "C"
