#pragma once

#include "unityDeltacast/unityDeltacast.h"
#include "capture/capture_state.hpp"

#include <array>
#include <atomic>
#include <string>

namespace deltacast {

// Per-stream state, one slot per capture index 0..3. Capture writes it; the frame-access
// exports and the recorder read it. Defined in unityDeltacast.cpp.
extern PublishedFrame publishedFrames[4];
extern std::array<std::atomic<int>, 4> width;
extern std::array<std::atomic<int>, 4> height;
extern std::array<std::atomic<bool>, 4> burnInFrameNumber;

// Status text returned by GetSignalAndVideoInfo().
void SetVideoInfo(int index, std::string text);

// Appends one line to the debug log returned by GetMessage().
void logHelper(const std::string& msg);

}

// Statement-like macro with file, line, and function
#define DC_LOG(MSG)                                                        \
    do {                                                                   \
        deltacast::logHelper( std::string("[") + __FILE__ + ":" +          \
                   std::to_string(__LINE__) + " " + __func__ + "] " +      \
                   std::string(MSG) );                                     \
    } while (0)
