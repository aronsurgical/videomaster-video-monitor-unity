# unityDeltacast test suite (doctest, vendored single header).
#
# Two executables, intentionally split:
#   * unityDeltacast_tests      ; black-box: links the built DLL and calls only the
#                                 exported C API. Never opens a device.
#   * unityDeltacast_tests_unit ; white-box unit tests of the header-only internals
#                                 (frame publication, worker lifecycle, pixel conversion).

# doctest is vendored as a single header; expose it as an INTERFACE target.
add_library(doctest_vendored INTERFACE)
target_include_directories(doctest_vendored INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)

# Black-box API tests
add_executable(unityDeltacast_tests
    doctest_main.cpp
    test_api.cpp
)
# Public headers arrive via unityDeltacast's PUBLIC include directory.
target_link_libraries(unityDeltacast_tests PRIVATE unityDeltacast doctest_vendored)
add_test(NAME unityDeltacast_tests COMMAND unityDeltacast_tests)

# White-box unit tests of header-only helpers
add_executable(unityDeltacast_tests_unit
    doctest_main.cpp
    test_capture_state.cpp
    test_pixel_convert.cpp
)
target_include_directories(unityDeltacast_tests_unit PRIVATE ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(unityDeltacast_tests_unit PRIVATE doctest_vendored)
add_test(NAME unityDeltacast_tests_unit COMMAND unityDeltacast_tests_unit)

# Emit the test exes into the same directory as unityDeltacast.dll, so the
# black-box test loads the DLL that was just built.
set_target_properties(unityDeltacast_tests unityDeltacast_tests_unit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set_tests_properties(unityDeltacast_tests unityDeltacast_tests_unit PROPERTIES TIMEOUT 15)
