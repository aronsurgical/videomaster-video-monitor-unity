cmake_minimum_required(VERSION 3.16)

include(cmake/version.cmake)
git_version(GIT_VERSION)
set_version(${GIT_VERSION})

project(
    "videomaster-video-monitor"
    VERSION ${PROJECT_VERSION}
    DESCRIPTION "Application that displays the content of an input stream captured by a Deltacast.TV device, interfaced with the VideoMaster SDK"
    LANGUAGES CXX
)

find_package(glfw3 REQUIRED)
if(TARGET glfw AND NOT TARGET glfw3::glfw3)
    add_library(glfw3::glfw3 ALIAS glfw)
endif()

add_subdirectory("src")
add_subdirectory("deps")
add_subdirectory(unityDeltacast)