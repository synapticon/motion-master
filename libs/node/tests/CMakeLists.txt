find_package(GTest CONFIG REQUIRED)

add_executable(mm_node_tests
    device_parameter_test.cc
    device_test.cc
    device_manager_test.cc
    process_image_test.cc
)

target_link_libraries(mm_node_tests PRIVATE mm::node GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_node_tests)
