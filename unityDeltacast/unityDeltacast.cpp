#include "unityDeltacast.h"

#include <string>
#include <atomic>
#include <thread>
#include <variant>
#include <mutex>

#include <VideoMasterCppApi/exception.hpp>
#include <VideoMasterCppApi/api.hpp>
#include <VideoMasterCppApi/board/board.hpp>
#include <VideoMasterCppApi/stream/sdi/sdi_stream.hpp>
#include <VideoMasterCppApi/slot/sdi/sdi_slot.hpp>
#include <VideoMasterCppApi/helper/sdi.hpp>

#include "../src/helper.hpp"

using namespace Application::Helper;

using namespace Deltacast::Wrapper;


static std::atomic<bool> running{ false };
static std::thread captureThread;

static std::vector<uint8_t> latestFrame;   // guarded by frameMutex
static std::mutex frameMutex;

static std::vector<uint8_t> bgra;  // scratch BGRA frame

// simple static message buffer
static std::string message = "Hello Deltacast DLL!\n";
static std::mutex  messageMutex;

static std::atomic<int> width{ 0 };
static std::atomic<int> height{ 0 };


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

using TechStream = std::variant<Deltacast::Wrapper::SdiStream, Deltacast::Wrapper::DvStream>;



static void log(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(messageMutex);
    if (message.size() < 2000) {
        message += msg + "\n";
    }
}

extern "C" {

    UNITYDLL_EXPORT void InitLibrary() {
        log("Library initialized!");
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

    UNITYDLL_EXPORT int GetWidth() {
        return width.load();
    }

    UNITYDLL_EXPORT int GetHeight() {
        return height.load();
    }


    UNITYDLL_EXPORT void StartCapture(int device_id, int rx_stream_id, int buffer_depth)
    {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return; // already running

        if (buffer_depth <= 0) buffer_depth = 8;

        captureThread = std::thread([device_id, rx_stream_id, buffer_depth]() {
            bool started = false;
            try {
                auto board = Board::open(device_id, nullptr);

                // open RX tech stream and get base stream
                auto rx_tech_stream = Application::Helper::open_stream(
                    board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
                auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);
                
                // 1) Wait until a signal is present on the connector
                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
                    throw std::runtime_error("No input detected on the requested RX");
                }

                // 2) Detect signal information (format, framerate, etc.)
                auto signal_information = Application::Helper::detect_information(rx_tech_stream);
                auto vc = Application::Helper::get_video_characteristics(signal_information);
                int W = vc.width, H = vc.height;
                width.store(W); height.store(H);

                //signal_information = SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };

                // 3) Queue depth & packing
                rx_stream.buffer_queue().set_depth(buffer_depth);
                rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);

                // 4) Configure stream to match the detected signal
                Application::Helper::configure_stream(rx_tech_stream, signal_information);



                // 5) Start the stream
                rx_stream.start();

                started = true;

                bgra.resize(size_t(width) * height * 4);
                log("width=" + std::to_string(width));
                log("height=" + std::to_string(height));

                while (running.load()) {
                    if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
                        continue;
                    }

                    // If signal changes mid-run, reconfigure
                    auto cur = Application::Helper::detect_information(rx_tech_stream);
                    if (cur != signal_information) {
                        if (started) { rx_stream.stop(); started = false; }
                        signal_information = cur;
                        vc = Application::Helper::get_video_characteristics(signal_information);
                        W = vc.width; H = vc.height;
                        width.store(W); height.store(H);
                        bgra.resize(size_t(W) * H * 4);

                        rx_stream.buffer_queue().set_depth(buffer_depth);
                        rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);
                        Application::Helper::configure_stream(rx_tech_stream, signal_information);
                        rx_stream.start();
                        started = true;
                        continue;
                    }

                    auto slot = rx_stream.pop_slot();
                    auto [src, totalBytes] = slot->video().buffer();

                    // derive source pitch (handles driver alignment)
                    if (height <= 0) {
                        continue;
                    }
                    int srcPitch = int(totalBytes / height);   // sanity: if (totalBytes % H) -> alignment mismatch
                    int dstPitch = width * 4;

                    UYVY_to_BGRA(src, srcPitch, bgra.data(), dstPitch, width, height, true);

                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        latestFrame = bgra;  // hand BGRA to Unity
                    }
                    //log("running");
                }

                if (started) {
                    rx_stream.stop();
                    started = false;
                }
                log("finished");
            }
            catch (const ApiException& e) {
                log(std::string("ApiException: ") + e.what());
            }
            catch (const std::exception& e) {
                log(std::string("std::exception: ") + e.what());
            }
            });
    }


    UNITYDLL_EXPORT void StopCapture()
    {
        running.store(false);
        if (captureThread.joinable()) {
            captureThread.join();
        }
    }

    UNITYDLL_EXPORT int GetFrame(uint8_t* dst, int maxSize)
    {
        std::lock_guard<std::mutex> lk(frameMutex);
        int n = std::min<int>(maxSize, latestFrame.size());
        if (n > 0) {
            std::memcpy(dst, latestFrame.data(), n);
        }
        return n;
    }

} // extern "C"
