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

# An XML Schema is the only machine check that a document conforms: pugixml does not validate, and a
# unit test can only assert the parts a reader would check by hand. Two documents are checked -- the
# ENI this library writes, and the vendor ESI it parses, which is the authority every parser test is
# written against. Both the schemas and xmllint are resolved at configure time, so a host without
# either gets no test rather than a test that silently passes.
find_program(XMLLINT_EXECUTABLE xmllint)
set(VALIDATE_XML "${CMAKE_CURRENT_SOURCE_DIR}/validate_xml.cmake")
set(ENI_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/data/eni/EtherCATConfig.xsd")
set(ESI_SCHEMA "${CMAKE_CURRENT_SOURCE_DIR}/data/esi/EtherCATInfo.xsd")

if(NOT XMLLINT_EXECUTABLE)
  message(STATUS "Schema validation tests disabled: xmllint was not found")
endif()

if(XMLLINT_EXECUTABLE AND EXISTS "${ENI_SCHEMA}")
  add_test(
    NAME EniSchemaValidation
    COMMAND
      ${CMAKE_COMMAND} -DXMLLINT=${XMLLINT_EXECUTABLE} -DSCHEMA=${ENI_SCHEMA}
      -DDOCUMENT=${CMAKE_CURRENT_BINARY_DIR}/eni_reference.xml -DTESTS=$<TARGET_FILE:mm_etg_tests>
      -DFILTER=EniWriterTest.WritesTheReferenceDocumentForSchemaValidation -P ${VALIDATE_XML})
elseif(XMLLINT_EXECUTABLE)
  message(STATUS "EniSchemaValidation disabled: ${ENI_SCHEMA} is not present")
endif()

if(XMLLINT_EXECUTABLE AND EXISTS "${ESI_SCHEMA}")
  add_test(
    NAME EsiSchemaValidation
    COMMAND ${CMAKE_COMMAND} -DXMLLINT=${XMLLINT_EXECUTABLE} -DSCHEMA=${ESI_SCHEMA}
            -DDOCUMENT=${CMAKE_CURRENT_SOURCE_DIR}/data/somanet-v5.6.6.xml -P ${VALIDATE_XML})
elseif(XMLLINT_EXECUTABLE)
  message(STATUS "EsiSchemaValidation disabled: ${ESI_SCHEMA} is not present")
endif()
