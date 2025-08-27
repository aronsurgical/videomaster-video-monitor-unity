#include "unityDeltacast.h"
#include <string>



#include <csignal>
#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <optional>
#include <variant>

#include <VideoMasterCppApi/exception.hpp>
#include <VideoMasterCppApi/to_string.hpp>
#include <VideoMasterCppApi/api.hpp>
#include <VideoMasterCppApi/board/board.hpp>
#include <VideoMasterCppApi/stream/sdi/sdi_stream.hpp>
#include <VideoMasterCppApi/slot/sdi/sdi_slot.hpp>

#include <VideoMasterCppApi/helper/sdi.hpp>
#include <mutex>

#include "../src/helper.cpp"
using namespace Application::Helper;

using namespace Deltacast::Wrapper;


static std::atomic<bool> running{ false };
static std::thread captureThread;
static std::vector<uint8_t> latestFrame;
static std::mutex frameMutex;

static std::vector<uint8_t> bgra;  // scratch BGRA frame
inline uint8_t clamp8(int x) { return (uint8_t)(x < 0 ? 0 : x>255 ? 255 : x); }

// UYVY (U Y0 V Y1) -> BGRA
static void UYVY_to_BGRA(const uint8_t* src, int srcPitch,
    uint8_t* dst, int dstPitch, int W, int H)
{
    for (int y = 0; y < H; ++y) {
        const uint8_t* s = src + y * srcPitch;
        uint8_t* d = dst + y * dstPitch;
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
                *d++ = (uint8_t)B; *d++ = (uint8_t)G; *d++ = (uint8_t)R; *d++ = 255; // BGRA
                };
            emit(Y0);
            emit(Y1);
        }
    }
}

using TechStream = std::variant<Deltacast::Wrapper::SdiStream, Deltacast::Wrapper::DvStream>;

// simple static message buffer
static std::string message = "Hello from my Unity DLL!";

extern "C" {

    UNITYDLL_EXPORT void InitLibrary() {
        message = "Library initialized!";
    }

    UNITYDLL_EXPORT int AddInts(int a, int b) {
        return a + b;
    }

    UNITYDLL_EXPORT const char* GetMessage() {
        return message.c_str();
    }

    UNITYDLL_EXPORT void StartCapture(int device_id, int rx_stream_id)
    {
        if (running) return;
        running = true;
        captureThread = std::thread([device_id, rx_stream_id]() {
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

                // 3) Configure stream to match the detected signal
                Application::Helper::configure_stream(rx_tech_stream, signal_information);

                // 4) Queue depth & packing
                rx_stream.buffer_queue().set_depth(8);
                rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);

                auto vc = Application::Helper::get_video_characteristics(signal_information);
                int W = vc.width, H = vc.height;
                bgra.resize(size_t(W) * H * 4);


                // 5) Start the stream
                rx_stream.start();

                while (running) {
                    if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running))
                        continue;

                    // If signal changes mid-run, reconfigure
                    if (Application::Helper::detect_information(rx_tech_stream) != signal_information) {
                        signal_information = Application::Helper::detect_information(rx_tech_stream);
                        Application::Helper::configure_stream(rx_tech_stream, signal_information);
                        continue;
                    }

                    auto slot = rx_stream.pop_slot();
                    auto [src, totalBytes] = slot->video().buffer();

                    // derive source pitch (handles driver alignment)
                    if (H <= 0) continue;
                    int srcPitch = int(totalBytes / H);   // sanity: if (totalBytes % H) -> alignment mismatch
                    int dstPitch = W * 4;

                    UYVY_to_BGRA(src, srcPitch, bgra.data(), dstPitch, W, H);

                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        latestFrame = bgra;  // hand BGRA to Unity
                    }
                    message = "running";
                }

                // optional: stop explicitly before destruction
                rx_stream.stop();
                message = "finished";
            }
            catch (const ApiException& e) {
                // handle errors/logging
                message = std::string("ApiException: ") + e.what();
            }
            catch (const std::exception& e) {
                message = std::string("std::exception: ") + e.what();
            }
            });
    }

    UNITYDLL_EXPORT void StopCapture()
    {
        running = false;
        if (captureThread.joinable())
            captureThread.join();
    }

    UNITYDLL_EXPORT int GetFrame(uint8_t* dst, int maxSize)
    {
        std::lock_guard<std::mutex> lock(frameMutex);
        int copySize = std::min<int>(maxSize, latestFrame.size());
        memcpy(dst, latestFrame.data(), copySize);
        return copySize; // number of bytes copied
    }



} // extern "C"