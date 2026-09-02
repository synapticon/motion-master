find_package(GTest CONFIG REQUIRED)

add_executable(
  mm_etg_tests
  eni_reader_test.cc
  eni_test.cc
  esi_data_type_test.cc
  esi_entry_test.cc
  esi_request_test.cc
  esi_somanet_test.cc
  esi_test.cc
  esi_unit_test.cc)

target_link_libraries(mm_etg_tests PRIVATE mm::etg GTest::gtest GTest::gtest_main)

# The real vendor ESI is too large to embed as a source literal, so esi_somanet_test.cc opens it
# from disk, and eni_test.cc writes the reference ENI for the schema-validation test below. Hand
# both directories to the test rather than letting it guess a path relative to cwd.
target_compile_definitions(
  mm_etg_tests PRIVATE MM_ETG_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
                       MM_ETG_TEST_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}")

# GCC 12-14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_etg_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                            -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_etg_tests DISCOVERY_TIMEOUT 60)

# The writer's own checks are not conformance, so the generated document is validated against the
# ENI XML Schema. The vendor ESI is validated too: every parser test is written against it, which
# makes it the authority this library is measured by. See cmake/schema_validation.cmake for why the
# schemas are not in the repository.
mm_add_schema_validation_test(
  NAME
  EniSchemaValidation
  SCHEMA
  ${MM_ENI_SCHEMA}
  DOCUMENT
  ${CMAKE_CURRENT_BINARY_DIR}/eni_reference.xml
  TESTS
  $<TARGET_FILE:mm_etg_tests>
  FILTER
  EniWriterTest.WritesTheReferenceDocumentForSchemaValidation)

mm_add_schema_validation_test(NAME EsiSchemaValidation SCHEMA ${MM_ESI_SCHEMA} DOCUMENT
                              ${CMAKE_CURRENT_SOURCE_DIR}/data/somanet-v5.6.6.xml)
