cmake_minimum_required(VERSION 3.16)

project(unityDeltacast LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "unityDeltacast is a Windows DLL (it uses the Win32 process/pipe API). Configure with MSVC, for example: cmake -S . -B build -G \"Visual Studio 18 2026\" -A x64")
endif()

# Project-local Find modules. The VideoMaster SDK is located through
# cmake/FindVideoMaster.cmake (VIDEOMASTER_ROOT, see the README).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

option(UNITYDELTACAST_BUILD_TESTS "Build unityDeltacast tests" ON)

# Dependencies
find_package(VideoMaster REQUIRED)

# Core library sources
set(LIB_SOURCES
    # Core: per-stream state shared by the modules + frame-access exports
    src/unityDeltacast.cpp

    # Capture module: capture workers (mono, field-mode bob, stereo) and their lifecycle
    src/capture/capture.cpp

    # Recording module: constant-fps writer threads feeding ffmpeg
    src/recording/ffmpeg_recorder.cpp

    # VideoMaster helpers: open/configure/detect streams (from DELTACAST's video-monitor sample)
    src/videomaster/videomaster_helpers.cpp
)

add_library(unityDeltacast SHARED ${LIB_SOURCES})

target_include_directories(unityDeltacast
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        # Unity native plugin API (IUnityRenderingExtensions.h), vendored from the Unity editor's PluginAPI
        ${CMAKE_CURRENT_SOURCE_DIR}/third_party/unity
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# The public header uses the VideoMaster C API enums, so the C API is PUBLIC.
target_link_libraries(unityDeltacast
    PUBLIC
        VideoMasterHD::Core
    PRIVATE
        VideoMasterCppApi
)

target_compile_definitions(unityDeltacast
    PRIVATE
        UNITYDELTACAST_BUILD
)

target_compile_options(unityDeltacast PRIVATE /permissive-)

# Copy the built DLL into Unity with tools/deploy_unity.ps1.

# Tests
# UNITYDELTACAST_BUILD_TESTS drives a CTest suite (see tests/CMakeLists.txt).
# None of the tests opens a device, so `ctest` passes with or without hardware.
if(UNITYDELTACAST_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
