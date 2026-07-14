find_package(GTest CONFIG REQUIRED)

add_executable(mm_comm_tests sii_test.cc)

target_link_libraries(mm_comm_tests PRIVATE mm::comm GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_comm_tests DISCOVERY_TIMEOUT 60)
