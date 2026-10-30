find_package(GTest CONFIG REQUIRED)

add_executable(
  mm_node_tests
  cia402_drive_test.cc
  cia402_test.cc
  config_file_test.cc
  device_parameter_test.cc
  object_addresses_test.cc
  device_test.cc
  device_manager_test.cc
  eni_collector_test.cc
  eni_request_test.cc
  esi_cache_test.cc
  firmware_package_test.cc
  hardware_description_test.cc
  integro_variant_test.cc
  ic_haus_registers_test.cc
  ic_mu_calibration_test.cc
  ic_mu_eeprom_test.cc
  ic_mu_encoder_test.cc
  kuebler_registers_test.cc
  monitoring_test.cc
  notification_bus_test.cc
  monitoring_manager_test.cc
  operation_modes_test.cc
  parameter_cache_test.cc
  parameter_refresher_test.cc
  procedure_registry_test.cc
  procedure_manager_test.cc
  procedure_test.cc
  process_data_dump_test.cc
  process_data_ring_test.cc
  process_image_test.cc
  concurrency_test.cc
  profile_device_test.cc
  profile_procedures_test.cc
  controller_tuning_test.cc
  plant_identification_test.cc
  somanet_drive_test.cc
  somanet_procedures_test.cc)

target_link_libraries(mm_node_tests PRIVATE mm::node mm::comm_spoe_fake GTest::gtest
                                            GTest::gtest_main)

# mu_3sl_test.cc checks that the vendored MU_3SL links and loads. On Windows, test discovery runs
# the test binary at build time, so the DLL must sit next to it by then.
if(MM_HAVE_MU_3SL)
  target_sources(mm_node_tests PRIVATE ic_mu_calibration_engine_test.cc mu_3sl_test.cc)
  mm_link_mu_3sl(mm_node_tests)
endif()

# The firmware package fixtures are read from the source tree, and eni_collector_test.cc writes the
# collected ENI for the schema-validation test below. esi_cache_test.cc reads the pinned SOMANET ESI
# of libs/etg. Hand every directory to the test rather than letting it guess a path relative to cwd.
target_compile_definitions(
  mm_node_tests
  PRIVATE MM_NODE_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
          MM_NODE_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}"
          MM_ETG_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/libs/etg/tests/data"
          MM_ENI_SAMPLES_DIR="${MM_ENI_SAMPLES_DIR}")

# GCC 12–14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_node_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                             -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_node_tests DISCOVERY_TIMEOUT 60)

# Collecting a real bus and writing it is the whole feature, so the document that comes out of the
# collector is validated against the ENI XML Schema too -- not only the writer's reference.
mm_add_schema_validation_test(
  NAME
  EniCollectorSchemaValidation
  SCHEMA
  ${MM_ENI_SCHEMA}
  DOCUMENT
  ${CMAKE_CURRENT_BINARY_DIR}/eni_collected.xml
  TESTS
  $<TARGET_FILE:mm_node_tests>
  FILTER
  EniCollectorTest.WritesTheCollectedDocumentForSchemaValidation)
