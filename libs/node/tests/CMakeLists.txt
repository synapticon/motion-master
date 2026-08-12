find_package(GTest CONFIG REQUIRED)

add_executable(
  mm_node_tests
  cia402_drive_test.cc
  cia402_test.cc
  device_parameter_test.cc
  object_addresses_test.cc
  device_test.cc
  device_manager_test.cc
  firmware_package_test.cc
  hardware_description_test.cc
  integro_variant_test.cc
  monitoring_test.cc
  monitoring_manager_test.cc
  operation_modes_test.cc
  parameter_cache_test.cc
  parameter_refresher_test.cc
  procedure_catalogue_test.cc
  procedure_manager_test.cc
  procedure_test.cc
  process_data_dump_test.cc
  process_data_ring_test.cc
  process_image_test.cc
  profile_device_test.cc
  profile_procedures_test.cc
  somanet_drive_test.cc
  somanet_procedures_test.cc)

target_link_libraries(mm_node_tests PRIVATE mm::node GTest::gtest GTest::gtest_main)

# The firmware package fixtures are read from the source tree rather than copied into the build
# directory, matching how mm_etg_tests reaches its ESI fixture.
target_compile_definitions(mm_node_tests
                           PRIVATE MM_NODE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

# GCC 12–14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_node_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                             -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_node_tests DISCOVERY_TIMEOUT 60)
