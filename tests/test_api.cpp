/* Black-box tests of the exported C API that need no device. Exercises argument
 * validation and the capture/recording lifecycle reachable without opening a board.
 * Links the built DLL; calls only the exported API. */

#include "unityDeltacast/unityDeltacast.h"
#include "doctest.h"

#include <array>
#include <climits>
#include <string>

namespace
{
void StartSdiCapture(int index, VHD_BUFFERPACKING packing)
{
    StartCapture(index, 0, 0, 2, 'S', 1920, 1080, 0, 50,
        VHD_DV_CS{}, VHD_DV_SAMPLING{}, VHD_VIDEOSTANDARD{}, VHD_CLOCKDIVISOR{},
        VHD_INTERFACE{}, UNITYDELTACAST_FIELD_MERGE, packing);
}

std::string Info(int index)
{
    char text[128] = "unchanged";
    GetSignalAndVideoInfo(index, text, sizeof(text));
    return text;
}
}

TEST_CASE("invalid stream indices return zero and touch nothing")
{
    std::array<uint8_t, 16> buffer;
    buffer.fill(0xCD);
    for (int index : {-1, 4, INT_MIN, INT_MAX})
    {
        CAPTURE(index);
        CHECK(GetWidth(index) == 0);
        CHECK(GetHeight(index) == 0);
        CHECK(GetNativeFrameCounter(index) == 0);
        CHECK(GetFrame(index, buffer.data(), static_cast<int>(buffer.size())) == 0);
        CHECK(StartRecording(index, "ffmpeg", "unused_%03d.mov", 25, 60, nullptr, 1) == 0);
        CHECK(IsRecording(index) == 0);
        CHECK(GetRecordedFrameNumber(index) == 0);
        CHECK(Info(index) == "unchanged");
        StopCapture(index);
        StopRecording(index);
        SetBurnInFrameNumber(index, 1);
        StartSdiCapture(index, VHD_BUFPACK_VIDEO_YUV422_8);
    }
    for (auto byte : buffer) CHECK(byte == 0xCD);
}

TEST_CASE("a stream without a capture serves no frame")
{
    std::array<uint8_t, 16> buffer;
    buffer.fill(0xCD);
    for (int index = 0; index < 4; ++index)
    {
        CAPTURE(index);
        CHECK(GetFrame(index, nullptr, 16) == 0);
        CHECK(GetFrame(index, buffer.data(), -1) == 0);
        CHECK(GetFrame(index, buffer.data(), 0) == 0);
        CHECK(GetFrame(index, buffer.data(), 16) == 0);
        StopCapture(index);
        CHECK(Info(index) == "Stopped");
    }
    for (auto byte : buffer) CHECK(byte == 0xCD);
}

TEST_CASE("StartCapture rejects every packing except UYVY8")
{
    StartSdiCapture(0, VHD_BUFPACK_VIDEO_RGB_32);
    CHECK(Info(0) == "Unsupported buffer packing: UYVY8 required");
    CHECK(GetWidth(0) == 0);
    CHECK(GetHeight(0) == 0);
    CHECK(GetNativeFrameCounter(0) == 0);
}

TEST_CASE("StopCapture also ends the stream's recording")
{
    // Without a capture the writer waits for a resolution, so no ffmpeg is launched.
    REQUIRE(StartRecording(1, "ffmpeg", "unused_%03d.mov", 25, 60, nullptr, 1) == 1);
    CHECK(StartRecording(1, "ffmpeg", "unused_%03d.mov", 25, 60, nullptr, 1) == 0);
    CHECK(IsRecording(1) == 1);
    StopCapture(1);
    CHECK(IsRecording(1) == 0);
}

TEST_CASE("a recorder still running at exit does not crash the process")
{
    // Deliberately left running: the DLL's static teardown must join it. Before that
    // fix the process died with 0xC0000409 on exit, which fails this test run.
    REQUIRE(StartRecording(2, "ffmpeg", "unused_%03d.mov", 25, 60, nullptr, 1) == 1);
}

TEST_CASE("the texture update callback always hands Unity pixels of the texture's size")
{
    UnityRenderingEventAndData callback = GetTextureUpdateCallback();
    REQUIRE(callback != nullptr);

    UnityRenderingExtTextureUpdateParamsV2 params{};
    params.width = 4;
    params.height = 2;
    params.bpp = 4;
    params.userData = 7;
    callback(kUnityRenderingExtEventUpdateTextureBeginV2, &params);
    CHECK(params.texData == nullptr);

    params.userData = 3;
    callback(kUnityRenderingExtEventUpdateTextureEndV2, &params);
    CHECK(params.texData == nullptr);

    callback(kUnityRenderingExtEventUpdateTextureBeginV2, &params);
    REQUIRE(params.texData != nullptr);
    const auto* pixels = static_cast<const uint8_t*>(params.texData);
    for (int i = 0; i < 4 * 2 * 4; ++i) CHECK(pixels[i] == 0);
    callback(kUnityRenderingExtEventUpdateTextureEndV2, &params);
}
