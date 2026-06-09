find_package(GTest CONFIG REQUIRED)

add_executable(mm_node_tests
    cia402_drive_test.cc
    cia402_test.cc
    device_parameter_test.cc
    device_test.cc
    device_manager_test.cc
    monitoring_test.cc
    monitoring_manager_test.cc
    parameter_refresher_test.cc
    process_data_ring_test.cc
    process_image_test.cc
    somanet_drive_test.cc
)

target_link_libraries(mm_node_tests PRIVATE mm::node GTest::gtest GTest::gtest_main)

# GCC 12–14 emit false-positive -Warray-bounds / -Wstringop-overflow inside
# libstdc++ <bits/stl_algobase.h> when test code is inlined at -O3 (release builds).
# Suppress for GCC on these test-only targets; shipped code keeps the warnings.
target_compile_options(mm_node_tests PRIVATE
    $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_node_tests DISCOVERY_TIMEOUT 60)
