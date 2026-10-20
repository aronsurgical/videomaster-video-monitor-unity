#pragma once

#include <stdint.h>
#include <VideoMasterHD_Core.h>
#include <VideoMasterHD_Dv.h>
#include <VideoMasterHD_Sdi.h>
#include <IUnityRenderingExtensions.h>

#ifdef _WIN32
#  ifdef UNITYDELTACAST_BUILD
#    define UNITYDLL_EXPORT __declspec(dllexport)
#  else
#    define UNITYDLL_EXPORT __declspec(dllimport)
#  endif
#else
#  define UNITYDLL_EXPORT
#endif

// StartCapture fieldMerge values. Existing field-merged calls keep using 1.
// For a 1080i50 SMPTE 425-1 Level-B dual-stream input, 0 selects SDK field
// mode and expands every captured field to a full-height frame with bob
// deinterlacing. For other input formats, 0 retains the previous unmerged frame
// behavior.
#define UNITYDELTACAST_FIELD_MODE_BOB 0u
#define UNITYDELTACAST_FIELD_MERGE    1u

// C API: functions must be extern "C" to avoid C++ name mangling
#ifdef __cplusplus
extern "C" {
#endif

UNITYDLL_EXPORT void StartCapture(int index, int device_id, int rx_stream_id, int buffer_depth,
    char inputType, unsigned int requested_width, unsigned int requested_height,
    unsigned int progressive, unsigned int framerate, VHD_DV_CS cable_color_space,
    VHD_DV_SAMPLING cable_sampling, VHD_VIDEOSTANDARD video_standard,
    VHD_CLOCKDIVISOR clock_divisor, VHD_INTERFACE video_interface, unsigned int fieldMerge,
    VHD_BUFFERPACKING buffer_packing);
UNITYDLL_EXPORT void StartCaptureStereo(int device_id, int rx_stream_id, int buffer_depth);
// Also stops the stream's recording, and clears its frame, size and counter.
UNITYDLL_EXPORT void StopCapture(int index);
UNITYDLL_EXPORT int GetWidth(int index);
UNITYDLL_EXPORT int GetHeight(int index);
// Returns the whole frame size, or zero without writing if unavailable or too small.
UNITYDLL_EXPORT int GetFrame(int index, uint8_t* dst, int maxSize);
UNITYDLL_EXPORT unsigned long long GetNativeFrameCounter(int index);
// Callback for CommandBuffer.IssuePluginCustomTextureUpdateV2 with userData = capture index: Unity's
// render thread uploads the newest published frame, so the copy and upload leave the main thread.
// A texture whose size differs from the frame keeps its previous contents.
UNITYDLL_EXPORT UnityRenderingEventAndData GetTextureUpdateCallback();
UNITYDLL_EXPORT void SetBurnInFrameNumber(int index, int enabled);

// Logs "Library initialized!"; Unity calls it first to check that the DLL loads.
UNITYDLL_EXPORT void InitLibrary();

// Returns and clears the debug log (caller copies it; valid until this thread's next call).
UNITYDLL_EXPORT const char* GetMessage();

// Copy a human-readable signal/video description for stream `index` into `buffer`
// (ANSI, null-terminated, truncated to bufferSize). Consumed by Unity's DeltacastAdapter.
// Also reports "No signal", "Capture error, retrying: ..." and "Stopped".
UNITYDLL_EXPORT void GetSignalAndVideoInfo(int index, char* buffer, int bufferSize);

// ---- Recording API (offloaded ffmpeg recording, see unityDeltacast.cpp) ----
// Start recording the BGRA frames currently being captured/published for `index`.
//   ffmpegExe      : "ffmpeg" (on PATH) or a full path to ffmpeg.exe
//   outputPattern  : segment output pattern, e.g. "C:\\rec\\Stream0_Video_part_%03d.mov"
//   fps            : constant output frame rate (e.g. 30 or 50)
//   segmentSeconds : seconds per segment (e.g. 60 -> exactly fps*60 frames per file)
//   firstPart      : number of the first segment file; a resumed recording continues there (>= 0)
//   encoderArgs    : optional encoder portion of the ffmpeg command; null/empty -> built-in default
//                    (keyframes at segment boundaries and the BT.601/709 color tag are always added)
//   applyVFlip     : 1 -> add "-vf vflip" (the published buffer is vertically flipped)
// Returns 1 on success (writer thread started), 0 on failure / already recording.
UNITYDLL_EXPORT int StartRecording(int index,
                                   const char* ffmpegExe,
                                   const char* outputPattern,
                                   int fps,
                                   int segmentSeconds,
                                   int firstPart,
                                   const char* encoderArgs,
                                   int applyVFlip);

// Stop recording for `index`: closes ffmpeg's stdin (finalizing the last segment),
// waits for ffmpeg to exit, and joins the writer thread.
UNITYDLL_EXPORT void StopRecording(int index);

// 1 while a recording session for `index` is active, else 0.
UNITYDLL_EXPORT int IsRecording(int index);

// Number of frames written to ffmpeg so far for this stream (paced, constant-FPS).
// This is the shared synchronization currency for Unity's .srt metadata.
UNITYDLL_EXPORT unsigned long long GetRecordedFrameNumber(int index);

#ifdef __cplusplus
}
#endif
