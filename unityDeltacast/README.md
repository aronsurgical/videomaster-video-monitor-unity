# Unity DELTACAST adapter

Build this directory directly with MSVC x64, CMake, and VideoMaster 6.35:

```text
cmake -S unityDeltacast -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DUNITYDELTACAST_BUILD_TESTS=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Run from an MSVC developer terminal using native Windows CMake/Ninja. Set
`VIDEOMASTER_ROOT` if the SDK is installed elsewhere. The DLL API test loads the
vendor runtime but does not open or enumerate devices. It exercises rejected
arguments; lifecycle and publication tests use synthetic workers and pixels.

Existing Unity export names and signatures are retained. Invalid stream indices
return zero or do nothing. `GetFrame` returns a complete frame's byte count, or
zero without writing when no frame is available, the output is null, or capacity
is insufficient. Callers must resize from `GetWidth`/`GetHeight` and retry.
Only `VHD_BUFPACK_VIDEO_YUV422_8` is accepted by `StartCapture`.

Capture start/stop and recorder thread ownership are serialized per stream.
Failed capture workers can restart after their previous thread has exited.
Restart clears the previous publication; an active recorder prevents restart.
The recorder copies pixels and the publication counter as one snapshot.
`GetMessage` returns storage valid until that thread's next `GetMessage` call.

The two-call Unity frame/counter API is still not an atomic snapshot. Counters
identify publications, not hardware timestamps or source-frame numbers. The
legacy stereo path still assumes its existing fixed input selection and only
detects signal changes on the first input; it is not qualified for arbitrary
stereo modes or mixing with other captures on the same physical channels.
Pixel conversion, vertical orientation, bob behavior and recording cadence are
unchanged. Stop/join and encoder shutdown have no guaranteed deadline; callers
must retain the existing quarantine protections and stop all work before DLL
unload. Device restart, field behavior, dual-stream capture and recording still
require hardware validation before deployment.
