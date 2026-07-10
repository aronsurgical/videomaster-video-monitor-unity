#pragma once

#ifdef _WIN32
#  define UNITYDLL_EXPORT __declspec(dllexport)
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
extern "C" {

// Example: initialize something
UNITYDLL_EXPORT void InitLibrary();

// Example: add two numbers
UNITYDLL_EXPORT int AddInts(int a, int b);

// Example: pass a string back to Unity (caller copies it!)
UNITYDLL_EXPORT const char* GetMessage();

// Copy a human-readable signal/video description for stream `index` into `buffer`
// (ANSI, null-terminated, truncated to bufferSize). Consumed by Unity's DeltacastAdapter.
UNITYDLL_EXPORT void GetSignalAndVideoInfo(int index, char* buffer, int bufferSize);

// ---- Recording API (offloaded ffmpeg recording, see unityDeltacast.cpp) ----
// Start recording the BGRA frames currently being captured/published for `index`.
//   ffmpegExe      : "ffmpeg" (on PATH) or a full path to ffmpeg.exe
//   outputPattern  : segment output pattern, e.g. "C:\\rec\\Stream0_Video_part_%03d.mov"
//   fps            : constant output frame rate (e.g. 30 or 50)
//   segmentSeconds : seconds per segment (e.g. 60 -> exactly fps*60 frames per file)
//   encoderArgs    : optional encoder portion of the ffmpeg command; null/empty -> built-in default
//   applyVFlip     : 1 -> add "-vf vflip" (the published buffer is vertically flipped)
// Returns 1 on success (writer thread started), 0 on failure / already recording.
UNITYDLL_EXPORT int StartRecording(int index,
                                   const char* ffmpegExe,
                                   const char* outputPattern,
                                   int fps,
                                   int segmentSeconds,
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

}