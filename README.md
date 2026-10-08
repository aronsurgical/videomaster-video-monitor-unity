# unityDeltacast

Windows DLL that captures up to four video streams from a DELTACAST card
(VideoMaster SDK) and hands them to Unity as BGRA frames. It can also record
each stream to segmented `.mov` files through an ffmpeg child process.
The Unity side is `DeltacastAdapter.cs` in atlasreborn.

## Public Header

[`include/unityDeltacast/unityDeltacast.h`](include/unityDeltacast/unityDeltacast.h):
a plain C API (`extern "C"`, cdecl). Every function takes a capture `index` 0..3;
invalid indices return zero or do nothing.

## Public API

### Capture

- `StartCapture(index, device, rx, buffer_depth, inputType, ...)`: mono capture.
  `inputType` `'A'` auto-detects the format and follows format changes. `'S'` / `'D'`
  force an SDI / DV format, which is kept even when detection disagrees
  (1080psf25 and 1080i50 look identical on the wire). Only
  `VHD_BUFPACK_VIDEO_YUV422_8` is accepted.
  `fieldMerge` `UNITYDELTACAST_FIELD_MERGE` (1) merges both fields into one frame.
  `UNITYDELTACAST_FIELD_MODE_BOB` (0) on a 1080i50 3G-B dual-stream input
  publishes each field as its own bob-deinterlaced frame (50 fps instead of 25).
- `StartCaptureStereo(device, rx, buffer_depth)`: side-by-side stereo
  (3840x1080) from a fixed 1080i50 3G-B dual-stream input. The left eye comes from
  `rx`, the right eye always from RX1. Publishes to index 0.
- `StopCapture(index)`: stops the capture **and its recording**, then clears
  the frame, the size and the counter.

### Frame access

- `GetWidth` / `GetHeight`: size of the published frame (0 before the first one).
- `GetFrame(index, dst, maxSize)`: copies the latest whole frame. Returns its
  byte count, or 0 without writing when there is no frame or `maxSize` is too
  small (resize from `GetWidth`/`GetHeight` and retry).
- `GetNativeFrameCounter`: number of frames published since `StartCapture`.
  It counts publications, not hardware timestamps.
- `SetBurnInFrameNumber(index, enabled)`: stamps that counter into the
  top-right corner of the frame (each eye in stereo).

### Recording

- `StartRecording(index, ffmpegExe, outputPattern, fps, segmentSeconds, encoderArgs, applyVFlip)`:
  a writer thread pipes the published frames to ffmpeg at a constant `fps`. When
  no new frame has arrived, it repeats the last one. Each segment holds exactly
  `fps × segmentSeconds` frames: keyframes are forced at the boundaries, even with
  custom `encoderArgs`. ffmpeg's output goes to `<outputPattern>.ffmpeg.log`.
- `StopRecording`, `IsRecording`, `GetRecordedFrameNumber` (the frame number the
  `.srt` metadata is keyed on).

### Diagnostics

- `GetSignalAndVideoInfo(index, buffer, size)`: the detected format, or
  `Starting`, `No signal`, `Capture error, retrying: ...`, `Stopped`.
- `GetMessage()`: returns and clears the debug log (capped at ~2000
  characters; the result is valid until this thread's next call).
- `InitLibrary()`: logs one line; Unity calls it first to check that the DLL loads.

## Runtime Model

- One worker thread per capture index. It runs capture *sessions*: open the
  board and stream, wait for a signal, configure, start, then pop, convert and
  publish frames. Any SDK error (slot timeout, cable pulled, failed detection,
  stream in use) ends only the current session, and a new one starts after
  500 ms. A lost signal shows `No signal` until it returns. The last frame stays
  published, so watch `GetNativeFrameCounter` or the status text to detect a
  frozen picture.
- Pixels are converted UYVY → BGRA with BT.709 for HD (≥ 720 lines) and BT.601
  for SD, and flipped vertically for Unity. The recorder encodes back to YUV with
  the same matrix and tags the file with it.
- A recording that sees the published size change stops itself
  (`IsRecording` becomes 0).
- Stop/join and the encoder shutdown have no deadline. Callers keep their
  timeout/quarantine protection and stop all work before the DLL is unloaded.

## Build

### Requirements (Windows)

- MSVC x64 + CMake 3.16+.
- DELTACAST VideoMaster SDK 6.30+ (tested with 6.35). Only `VideoMasterHD.dll`
  is needed at runtime; the VideoMaster installer puts it in `System32`.

### Configure + build

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

This produces `build\Release\unityDeltacast.dll`. Ninja from a developer
prompt works as well (`-G Ninja -DCMAKE_BUILD_TYPE=Release`). Pass
`-DUNITYDELTACAST_BUILD_TESTS=OFF` to skip the tests.

## Environment Variables

| Variable | Notes |
|---|---|
| `VIDEOMASTER_ROOT` | VideoMaster SDK install root (default `C:\Program Files\DELTACAST\VideoMaster`), read by [`cmake/FindVideoMaster.cmake`](cmake/FindVideoMaster.cmake). Also accepted as `-DVIDEOMASTER_ROOT=`. |

## Tests

`ctest` runs two doctest executables; neither opens a device:

- `unityDeltacast_tests`: black-box tests of the exported API (argument
  validation, `StopCapture` ending a recording, clean process exit with a
  recorder still running).
- `unityDeltacast_tests_unit`: frame publication and worker lifecycle
  (`src/capture/capture_state.hpp`), and pixel conversion: color matrices, bob
  parity/flip, padded fields, burn-in clipping (`src/image/pixel_convert.hpp`).

Capture, field mode, stereo and recording also need a check on the card before
deploying.

## Deploying to Unity

[`tools/deploy_unity.ps1`](tools/deploy_unity.ps1) checks the built DLL (its
exports and that it imports only Windows, VC++ redistributable and VideoMaster
DLLs). It writes `unity-dll-manifest.txt` and syncs the DLL into
`Assets\Plugins\unityDeltacast` of the atlasreborn project. Without `-Apply` it
only reports what it would do. Close the Unity editor first, because it locks
the DLL.

```powershell
.\tools\deploy_unity.ps1            # dry-run report
.\tools\deploy_unity.ps1 -Apply     # copy into Unity
```

The checked-in manifest describes the last deployed binary.

## Layout

```text
cmake/FindVideoMaster.cmake      SDK discovery (C API + C++ wrapper build)
include/unityDeltacast/          public C API
src/unityDeltacast.cpp           shared per-stream state, frame access, log
src/capture/                     capture workers + publication/lifecycle state
src/image/                       UYVY -> BGRA, bob deinterlacing, burn-in
src/recording/                   ffmpeg writer threads
src/videomaster/                 stream open/configure/detect helpers
tests/                           doctest suites
tools/deploy_unity.ps1           Unity deployment
```

`src/videomaster/videomaster_helpers.*` derive from DELTACAST's
videomaster-video-monitor sample (Apache-2.0, see [LICENSE](LICENSE)).
