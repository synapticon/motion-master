find_package(GTest CONFIG REQUIRED)

add_executable(mm_capture_tests ethercat_frame_test.cc pcap_test.cc summary_test.cc)

target_link_libraries(mm_capture_tests PRIVATE mm::capture GTest::gtest GTest::gtest_main)

# A capture recorded on the bench is the fixture the summary is checked against, and the pcap tests
# write a file of their own. Hand both directories to the test rather than letting it guess a path
# relative to cwd.
target_compile_definitions(
  mm_capture_tests PRIVATE MM_CAPTURE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                           MM_CAPTURE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

include(GoogleTest)
gtest_discover_tests(mm_capture_tests DISCOVERY_TIMEOUT 60)
