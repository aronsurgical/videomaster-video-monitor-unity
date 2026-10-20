#include "unityDeltacast_internal.hpp"
#include "capture/capture_state.hpp"
#include "image/pixel_convert.hpp"

#include <chrono>
#include <initializer_list>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <VideoMasterCppApi/exception.hpp>
#include <VideoMasterCppApi/api.hpp>
#include <VideoMasterCppApi/board/board.hpp>
#include <VideoMasterCppApi/stream/sdi/sdi_stream.hpp>
#include <VideoMasterCppApi/slot/sdi/sdi_slot.hpp>
#include <VideoMasterCppApi/helper/sdi.hpp>

#include "videomaster/videomaster_helpers.hpp"

using namespace Application::Helper;

using namespace Deltacast::Wrapper;

using namespace deltacast;

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

static CaptureWorker captureWorkers[4];

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
                         std::initializer_list<std::pair<int, int>> regions, int64_t arrivalHostUs)
{
    if (burnInFrameNumber[index].load(std::memory_order_relaxed)) {
        const unsigned long long frameNo = publishedFrames[index].sequence() + 1;
        for (const auto& [x, w] : regions)
            BurnFrameNumberBGRA(frame.data(), W, H, W * 4, frameNo, x, w, true);
    }
    publishedFrames[index].publish(frame, arrivalHostUs);
}


extern "C" {

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
    if (!valid_index(index)) {
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
            const auto& matrix = MatrixForHeight(H);
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
                const int64_t arrivalHostUs = HostNowUs();
                auto [src, totalBytes] = slot->video().buffer();

                if (useFieldModeBob) {
                    const bool evenField = (slot->parity() == Slot::Parity::EVEN);
                    if (!UYVY_Field_to_BGRA_Bob(src, totalBytes, frame.data(), dstPitch,
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
                    UYVY_to_BGRA(src, int(totalBytes / H), frame.data(), dstPitch, W, H, true, matrix);
                }

                PublishFrame(index, frame, W, H, { {0, W} }, arrivalHostUs);
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
            const auto [outW, outH] = stereo_extent(W, H, W, H);

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
            const auto& matrix = MatrixForHeight(H);
            DC_LOG("stereo: capturing " + std::to_string(outW) + "x" + std::to_string(outH));

            while (captureWorkers[0].running.load()) {
                if (!rx.signal_present()) return;

                auto leftSlot = left.pop_slot();
                auto rightSlot = right.pop_slot();
                const int64_t arrivalHostUs = HostNowUs();
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
                    UYVY_to_BGRA(src, int(totalBytes / H), eyeOrigin, pitch, W, H, true, matrix);
                    BobFillMissingRows(eyeOrigin, W, H, pitch, keptParity);
                }
                if (!complete) continue;

                PublishFrame(0, frame, outW, outH, { {0, W}, {W, W} }, arrivalHostUs);
            }
        });
    });
}

UNITYDLL_EXPORT void StopCapture(int index)
{
    if (!valid_index(index)) return;
    // A recording never outlives its capture; it would only repeat the last frame.
    StopRecording(index);
    captureWorkers[index].stop([index] { ResetStream(index, "Stopped"); });
}

} // extern "C"
