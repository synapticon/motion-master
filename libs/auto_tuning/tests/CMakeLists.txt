find_package(GTest CONFIG REQUIRED)

add_executable(mm_auto_tuning_tests process_test.cc)

target_link_libraries(mm_auto_tuning_tests PRIVATE mm::auto_tuning GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_auto_tuning_tests DISCOVERY_TIMEOUT 60)
