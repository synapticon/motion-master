find_package(GTest CONFIG REQUIRED)

add_executable(mm_api_tests router_test.cc)

target_link_libraries(mm_api_tests PRIVATE mm::api GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_api_tests DISCOVERY_TIMEOUT 60)
