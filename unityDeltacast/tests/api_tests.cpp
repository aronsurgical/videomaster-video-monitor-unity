#include "../unityDeltacast.h"
#include <array>
#include <climits>
#include <iostream>

int main() {
    std::array<uint8_t, 16> buffer;
    buffer.fill(0xCD);
    for (int index : {-1, 4, INT_MIN, INT_MAX}) {
        if (GetWidth(index) || GetHeight(index) || GetNativeFrameCounter(index) ||
            GetFrame(index, buffer.data(), static_cast<int>(buffer.size()))) return 1;
        StopCapture(index);
        StopRecording(index);
        SetBurnInFrameNumber(index, 1);
        StartCapture(index, 0, 0, 2, 'S', 1920, 1080, 0, 50,
            VHD_DV_CS{}, VHD_DV_SAMPLING{}, VHD_VIDEOSTANDARD{}, VHD_CLOCKDIVISOR{},
            VHD_INTERFACE{}, 0, VHD_BUFPACK_VIDEO_YUV422_8);
    }
    for (int index = 0; index < 4; ++index) {
        if (GetFrame(index, nullptr, 16) || GetFrame(index, buffer.data(), -1) ||
            GetFrame(index, buffer.data(), 0) || GetFrame(index, buffer.data(), 16)) return 2;
        StopCapture(index);
        StartCapture(index, 0, 0, 2, 'S', 1920, 1080, 0, 50,
            VHD_DV_CS{}, VHD_DV_SAMPLING{}, VHD_VIDEOSTANDARD{}, VHD_CLOCKDIVISOR{},
            VHD_INTERFACE{}, 0, VHD_BUFPACK_VIDEO_RGB_32);
        if (GetWidth(index) || GetHeight(index) || GetNativeFrameCounter(index)) return 3;
    }
    for (auto byte : buffer) if (byte != 0xCD) return 4;
    std::cout << "hardware-free DLL API tests passed\n";
    return 0;
}
