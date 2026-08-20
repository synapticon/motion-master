find_package(GTest CONFIG REQUIRED)

add_executable(mm_net_tests http_client_test.cc)

target_link_libraries(mm_net_tests PRIVATE mm::net GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_net_tests DISCOVERY_TIMEOUT 60)
