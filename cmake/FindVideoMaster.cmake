# FindVideoMaster.cmake
#
# Locates the DELTACAST VideoMaster SDK. Wraps the vendor-supplied
# VideoMasterHDConfig.cmake (C API) and builds the C++ wrapper from the
# sources the SDK ships in cpp_wrapper/.
#
# Hints (in priority order):
#   1. -DVIDEOMASTER_ROOT=<path>     (CMake variable / cache)
#   2. $ENV{VIDEOMASTER_ROOT}        (terminal environment)
#   3. C:/Program Files/DELTACAST/VideoMaster   (Windows default)
#
# On success sets:
#   VideoMaster_FOUND
#   VideoMasterHD::Core   (imported target: VideoMasterHD.lib + C API headers)
#   VideoMasterCppApi     (static library target of the C++ wrapper)

include(FindPackageHandleStandardArgs)

if(NOT VIDEOMASTER_ROOT)
    if(DEFINED ENV{VIDEOMASTER_ROOT})
        set(VIDEOMASTER_ROOT "$ENV{VIDEOMASTER_ROOT}")
    else()
        set(VIDEOMASTER_ROOT "C:/Program Files/DELTACAST/VideoMaster")
    endif()
endif()

find_package(VideoMasterHD CONFIG QUIET
    PATHS "${VIDEOMASTER_ROOT}/cmake"
    NO_DEFAULT_PATH
)

find_path(VideoMaster_CPP_WRAPPER_DIR
    NAMES include/VideoMasterCppApi/api.hpp
    HINTS "${VIDEOMASTER_ROOT}/cpp_wrapper"
    NO_DEFAULT_PATH
)

find_package_handle_standard_args(VideoMaster
    REQUIRED_VARS VideoMasterHD_FOUND VideoMaster_CPP_WRAPPER_DIR
    FAIL_MESSAGE "VideoMaster SDK not found. Set VIDEOMASTER_ROOT (env var or -DVIDEOMASTER_ROOT) to the install root containing cmake/VideoMasterHDConfig.cmake and cpp_wrapper/."
)

# The wrapper's own CMakeLists reuses the VideoMasterHD package found above.
if(VideoMaster_FOUND AND NOT TARGET VideoMasterCppApi)
    add_subdirectory("${VideoMaster_CPP_WRAPPER_DIR}" "${CMAKE_BINARY_DIR}/vm_cpp_wrapper_build" EXCLUDE_FROM_ALL)
endif()

mark_as_advanced(VideoMaster_CPP_WRAPPER_DIR)
