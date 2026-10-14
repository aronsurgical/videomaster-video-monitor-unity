#include "unityDeltacast_internal.hpp"

#include <cstring>
#include <mutex>

namespace deltacast {

PublishedFrame publishedFrames[4];

std::array<std::atomic<int>, 4> width = {
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0}
};

std::array<std::atomic<int>, 4> height = {
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0},
    std::atomic<int>{0}
};

std::array<std::atomic<bool>, 4> burnInFrameNumber = {
    std::atomic<bool>{false},
    std::atomic<bool>{false},
    std::atomic<bool>{false},
    std::atomic<bool>{false}
};

// Per-stream human-readable signal/video description, exposed via GetSignalAndVideoInfo().
// (Restored from commit 9a1e1a8, which was dropped by a later merge.)
static std::array<std::string, 4> videoInfo;
static std::array<std::mutex, 4>  videoInfoMutex;

void SetVideoInfo(int index, std::string text) {
    if (!valid_index(index)) return;
    std::lock_guard<std::mutex> lk(videoInfoMutex[index]);
    videoInfo[index] = std::move(text);
}

// simple static message buffer
static std::string message = "Hello Deltacast DLL!\n";
static std::mutex  messageMutex;

void logHelper(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(messageMutex);
    if (message.size() < 2000) {
        message += msg + "\n";
    }
}

}

using namespace deltacast;

// Render thread only. Unity reads texData between Begin and End, so the frame is copied out of
// the published buffer rather than holding its lock across the upload.
static std::vector<uint8_t> uploadStaging[4];
static uint64_t uploadSeen[4];

static void UNITY_INTERFACE_API OnTextureUpdate(int eventId, void* data)
{
    if (eventId != kUnityRenderingExtEventUpdateTextureBeginV2 || !data) return;
    auto* params = static_cast<UnityRenderingExtTextureUpdateParamsV2*>(data);
    const int index = static_cast<int>(params->userData);
    if (!valid_index(index)) return;
    const size_t bytes = size_t(params->width) * params->height * params->bpp;
    std::vector<uint8_t>& staging = uploadStaging[index];
    publishedFrames[index].copy_new(uploadSeen[index], bytes, staging);
    // Unity requires texData; a texture the frame does not fit gets its previous (or black) pixels.
    if (staging.size() != bytes) staging.assign(bytes, 0);
    params->texData = staging.data();
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
        return valid_index(index) ? width[index].load() : 0;
    }

    UNITYDLL_EXPORT int GetHeight(int index) {
        return valid_index(index) ? height[index].load() : 0;
    }

    UNITYDLL_EXPORT void SetBurnInFrameNumber(int index, int enabled)
    {
        if (!valid_index(index)) return;

        burnInFrameNumber[index].store(enabled != 0, std::memory_order_relaxed);
    }

    UNITYDLL_EXPORT unsigned long long GetNativeFrameCounter(int index)
    {
        if (!valid_index(index)) return 0;

        return publishedFrames[index].sequence();
    }

    UNITYDLL_EXPORT void GetSignalAndVideoInfo(int index, char* buffer, int bufferSize) {
        if (!valid_index(index) || !buffer || bufferSize <= 0)
            return;

        std::lock_guard<std::mutex> lk(videoInfoMutex[index]);

        strncpy(buffer, videoInfo[index].c_str(), bufferSize - 1);
        buffer[bufferSize - 1] = '\0';
    }

    UNITYDLL_EXPORT int GetFrame(int index, uint8_t* dst, int maxSize)
    {
        if (!valid_index(index)) return 0;
        return publishedFrames[index].copy(dst, maxSize);
    }

    UNITYDLL_EXPORT UnityRenderingEventAndData GetTextureUpdateCallback()
    {
        return OnTextureUpdate;
    }

} // extern "C"
