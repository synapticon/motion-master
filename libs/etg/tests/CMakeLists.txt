find_package(GTest CONFIG REQUIRED)

add_executable(
  mm_etg_tests
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

# The ENI XML Schema is the only machine check that the writer produces a conformant document: the
# unit tests can only assert the parts a reader would check by hand, and pugixml cannot validate
# against a schema. Both the schema and xmllint are resolved at configure time, so a host without
# either gets no test rather than a test that silently passes.
find_program(XMLLINT_EXECUTABLE xmllint)
set(ENI_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/data/eni/EtherCATConfig.xsd")
if(XMLLINT_EXECUTABLE AND EXISTS "${ENI_SCHEMA}")
  add_test(
    NAME EniSchemaValidation
    COMMAND
      ${CMAKE_COMMAND} -DTESTS=$<TARGET_FILE:mm_etg_tests> -DXMLLINT=${XMLLINT_EXECUTABLE}
      -DSCHEMA=${ENI_SCHEMA} -DDOCUMENT=${CMAKE_CURRENT_BINARY_DIR}/eni_reference.xml -P
      ${CMAKE_CURRENT_SOURCE_DIR}/validate_eni.cmake)
elseif(NOT XMLLINT_EXECUTABLE)
  message(STATUS "EniSchemaValidation disabled: xmllint was not found")
else()
  message(STATUS "EniSchemaValidation disabled: ${ENI_SCHEMA} is not present")
endif()
