find_package(GTest CONFIG REQUIRED)

add_executable(mm_core_tests version_test.cc util_test.cc system_info_test.cc cyclic_timer_test.cc)

target_link_libraries(mm_core_tests PRIVATE mm::core GTest::gtest GTest::gtest_main)

# GCC 12–14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_core_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                             -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_core_tests DISCOVERY_TIMEOUT 60)
