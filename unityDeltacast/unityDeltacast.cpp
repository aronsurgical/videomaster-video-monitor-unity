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

// Statement-like macro with file, line, and function
#define DC_LOG(MSG)                                                        \
    do {                                                                   \
        logHelper( std::string("[") + __FILE__ + ":" +                     \
                   std::to_string(__LINE__) + " " + __func__ + "] " +      \
                   std::string(MSG) );                                     \
    } while (0)


using namespace Application::Helper;

using namespace Deltacast::Wrapper;


static std::atomic<bool> running{ false };
static std::thread captureThread;

static std::vector<uint8_t> latestFrame;   // guarded by frameMutex
static std::mutex frameMutex;



static std::vector<uint8_t> bgra;  // scratch BGRA frame
static std::vector<uint8_t> bgra2;  // scratch BGRA frame

// simple static message buffer
static std::string message = "Hello Deltacast DLL!\n";
static std::mutex  messageMutex;

static std::atomic<int> width{ 0 };
static std::atomic<int> width2{ 0 };
static std::atomic<int> height{ 0 };
static std::atomic<int> height2{ 0 };


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

void StartCaptureAuto(int device_id, int rx_stream_id, int buffer_depth)
{
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) return; // already running

    if (buffer_depth <= 0) {
        buffer_depth = 8;
    }

    captureThread = std::thread([device_id, rx_stream_id, buffer_depth]() {
        bool started = false;
        try {
            auto board = Board::open(device_id, nullptr);

            // open RX tech stream and get base stream
            auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
            auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);

            // 1) Wait until a signal is present on the connector
            if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
                throw std::runtime_error("No input detected on the requested RX");
            }

            // 2) Detect signal information (format, framerate, etc.)
            auto signal_information = Application::Helper::detect_information(rx_tech_stream);
            //signal_information = SdiSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
            //signal_information = DvSignalInformation{ VHD_VIDEOSTD_S274M_1080i_50Hz, VHD_CLOCKDIV_1, VHD_INTERFACE_3G_B_DS_425_1 };
            auto vc = Application::Helper::get_video_characteristics(signal_information);
            int W = vc.width;
            int H = vc.height;
            width.store(W);
            height.store(H);


            // 3) Queue depth & packing
            rx_stream.buffer_queue().set_depth(buffer_depth);
            rx_stream.set_buffer_packing(VHD_BUFPACK_VIDEO_YUV422_8);

            //field merge necesary for example in stereo dual stream
            //rx_stream.enable_field_merge();

            // 4) Configure stream to match the detected signal
            Application::Helper::configure_stream(rx_tech_stream, signal_information);



            // 5) Start the stream
            rx_stream.start();

            started = true;

            bgra.resize(size_t(width) * height * 4);

            DC_LOG("width=" + std::to_string(width));
            DC_LOG("height=" + std::to_string(height));

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

    UNITYDLL_EXPORT int GetWidth() {
        return width.load();
    }

    UNITYDLL_EXPORT int GetHeight() {
        return height.load();
    }


    UNITYDLL_EXPORT void StartCapture(int device_id, int rx_stream_id, int buffer_depth, char inputType, unsigned int requested_width, unsigned int requested_height, unsigned int progressive, unsigned int framerate, VHD_DV_CS cable_color_space, VHD_DV_SAMPLING cable_sampling, VHD_VIDEOSTANDARD video_standard, VHD_CLOCKDIVISOR clock_divisor, VHD_INTERFACE video_interface, unsigned int fieldMerge, VHD_BUFFERPACKING buffer_packing)
    {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return; // already running

        if (buffer_depth <= 0) {
            buffer_depth = 8;
        }

        captureThread = std::thread([device_id, rx_stream_id, buffer_depth, inputType, requested_width, requested_height, progressive, framerate, cable_color_space, cable_sampling, video_standard, clock_divisor, video_interface, fieldMerge, buffer_packing]() {
            bool started = false;
            try {
                auto board = Board::open(device_id, nullptr);

                // open RX tech stream and get base stream
                auto rx_tech_stream = Application::Helper::open_stream(board, Application::Helper::rx_index_to_streamtype(rx_stream_id));
                auto& rx_stream = Application::Helper::to_base_stream(rx_tech_stream);

                // 1) Wait until a signal is present on the connector
                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
                    throw std::runtime_error("No input detected on the requested RX");
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
                width.store(W);
                height.store(H);


                // 3) Queue depth & packing
                rx_stream.buffer_queue().set_depth(buffer_depth);
                rx_stream.set_buffer_packing(buffer_packing);

                //field merge necesary for example in stereo dual stream
                DC_LOG("fieldMerge=" + std::to_string(fieldMerge));
                if (fieldMerge != 0) {
                    rx_stream.enable_field_merge();
                }

                // 4) Configure stream to match the detected signal
                Application::Helper::configure_stream(rx_tech_stream, signal_information);



                // 5) Start the stream
                rx_stream.start();

                started = true;

                bgra.resize(size_t(width) * height * 4);

                DC_LOG("width=" + std::to_string(width));
                DC_LOG("height=" + std::to_string(height));

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
                        rx_stream.set_buffer_packing(buffer_packing);
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

    UNITYDLL_EXPORT void StartCaptureStereo(int device_id, int rx_stream_id, int buffer_depth)
    {
        bool expected = false;
        if (!running.compare_exchange_strong(expected, true)) return; // already running

        if (buffer_depth <= 0) {
            buffer_depth = 8;
        }

        captureThread = std::thread([device_id, rx_stream_id, buffer_depth]() {
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
                if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
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

                width.store(outW);
                height.store(outH);

                // allocate working & output buffers
                bgra.resize(size_t(W) * H * 4);
                bgra2.resize(size_t(W2) * H2 * 4);
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

                while (running.load()) {
                    if (!Application::Helper::wait_for_input(board.rx(rx_stream_id), running)) {
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
                    UYVY_to_BGRA(src, srcPitch, bgra.data(), dstPitch, W, H, true);
                    UYVY_to_BGRA(src2, srcPitch2, bgra2.data(), dstPitch2, W2, H2, true);

                    bool topFieldFirst = (slot->parity() == Slot::Parity::EVEN);
                    // pack into one wide texture
                    PackSideBySide_BGRA_deinterlaced(bgra.data(), W, H, dstPitch,
                        bgra2.data(), W2, H2, dstPitch2,
                        sbsBGRA.data(), outW, outH, topFieldFirst);

                    // publish the single combined frame
                    {
                        std::lock_guard<std::mutex> lock(frameMutex);
                        latestFrame = sbsBGRA;
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
