cmake_minimum_required(VERSION 3.16)

project(unityDeltacast LANGUAGES CXX)

add_library(unityDeltacast SHARED
    unityDeltacast.cpp
    unityDeltacast.h
)

# ---- SDK layout (from your listing) ----
set(VIDEOMASTER_ROOT "C:/Program Files/DELTACAST/VideoMaster")
set(VM_INCLUDE_CPP   "${VIDEOMASTER_ROOT}/cpp_wrapper/include")
set(VM_INCLUDE_C     "${VIDEOMASTER_ROOT}/resources/include")
set(VM_LIB_DIR       "${VIDEOMASTER_ROOT}/resources/lib")

# ---- Try to load C API package (may or may not define imported targets) ----
# If the folder doesn't exist on some machines, this will be harmlessly skipped.
if(EXISTS "${VIDEOMASTER_ROOT}/cmake")
  set(VideoMasterHD_DIR "${VIDEOMASTER_ROOT}/cmake")
  find_package(VideoMasterHD QUIET)   # don't fail hard; we have a fallback
endif()

# ---- Build the C++ wrapper ONCE (guard avoids duplicate target errors) ----
if(NOT TARGET VideoMasterCppApi)
  add_subdirectory(
    "${VIDEOMASTER_ROOT}/cpp_wrapper"
    "${CMAKE_BINARY_DIR}/vm_cpp_wrapper_build"
  )
endif()

# ---- Include dirs (explicit, so headers always resolve) ----
target_include_directories(unityDeltacast PRIVATE
  "${VM_INCLUDE_CPP}"
  "${VM_INCLUDE_C}"
  "${CMAKE_CURRENT_SOURCE_DIR}"
)

# ---- Link to wrapper target (it’s a real CMake target we just added) ----
target_link_libraries(unityDeltacast PRIVATE VideoMasterCppApi)

# ---- Link to C API:
# Prefer imported targets from find_package(VideoMasterHD).
# If not available, fall back to linking libs from resources/lib.
if(TARGET VideoMasterHD)
  target_link_libraries(unityDeltacast PRIVATE
    VideoMasterHD
  )
  if(TARGET VideoMasterHD_Audio)
    target_link_libraries(unityDeltacast PRIVATE VideoMasterHD_Audio)
  endif()
  if(TARGET VideoMasterHD_Vbi)
    target_link_libraries(unityDeltacast PRIVATE VideoMasterHD_Vbi)
  endif()
  if(TARGET VideoMasterHD_VbiData)
    target_link_libraries(unityDeltacast PRIVATE VideoMasterHD_VbiData)
  endif()
else()
  # Fallback: link directly from lib dir
  target_link_directories(unityDeltacast PRIVATE "${VM_LIB_DIR}")
  # Use bare names (MSVC will look for .lib in VM_LIB_DIR) or absolute paths:
  target_link_libraries(unityDeltacast PRIVATE
    "${VM_LIB_DIR}/VideoMasterHD.lib"
    "${VM_LIB_DIR}/VideoMasterHD_Audio.lib"
    "${VM_LIB_DIR}/VideoMasterHD_Vbi.lib"
    "${VM_LIB_DIR}/VideoMasterHD_VbiData.lib"
  )
endif()

# ---- Output locations ----
set_target_properties(unityDeltacast PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# ---- Language / MSVC sanity ----
target_compile_features(unityDeltacast PUBLIC cxx_std_17)
set_target_properties(unityDeltacast PROPERTIES CXX_EXTENSIONS OFF)

if(MSVC)
  # Make sure nothing compiles as C by accident
  target_compile_options(unityDeltacast PRIVATE /permissive-)
  if(TARGET VideoMasterCppApi)
    target_compile_options(VideoMasterCppApi PRIVATE /TP)
  endif()
endif()

# ---- (Optional) copy runtime DLLs next to your DLL for testing/Unity ----
# set(RUNTIME_OUT "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
# add_custom_command(TARGET unityDeltacast POST_BUILD
#   COMMAND ${CMAKE_COMMAND} -E copy_if_different
#     "${VM_LIB_DIR}/VideoMasterHD.dll"
#     "${VM_LIB_DIR}/VideoMasterHD_Audio.dll"
#     "${VM_LIB_DIR}/VideoMasterHD_Vbi.dll"
#     "${VM_LIB_DIR}/VideoMasterHD_VbiData.dll"
#     "${RUNTIME_OUT}"
# )
