find_package(GTest CONFIG REQUIRED)

add_executable(
  mm_etg_tests
  esi_data_type_test.cc
  esi_entry_test.cc
  esi_request_test.cc
  esi_somanet_test.cc
  esi_test.cc
  esi_unit_test.cc
  fsoe_crc_test.cc
  fsoe_frame_test.cc
  fsoe_master_interop_test.cc
  fsoe_master_test.cc)

target_link_libraries(mm_etg_tests PRIVATE mm::etg GTest::gtest GTest::gtest_main)

# The real vendor ESI is too large to embed as a source literal, so esi_somanet_test.cc opens it
# from disk. Hand the directory to the test rather than letting it guess a path relative to cwd.
target_compile_definitions(mm_etg_tests
                           PRIVATE MM_ETG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# GCC 12-14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_etg_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                            -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_etg_tests DISCOVERY_TIMEOUT 60)
