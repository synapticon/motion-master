# Locates the ETG XML Schemas and xmllint, for the tests that validate a document against them.
#
# pugixml does not validate against a schema, so xmllint is what proves conformance: for the ENI
# this project writes and collects, and for the vendor ESI it parses.
#
# The schemas are not in the repository. ETG's download terms forbid copying, distributing or
# mirroring their files without written permission, and this repository is public. So the paths
# below are defaults, not contents: put EtherCATConfig.xsd in libs/etg/tests/data/eni/ and
# EtherCATInfo.xsd with EtherCATBase.xsd beside it in libs/etg/tests/data/esi/ -- both directories
# are gitignored -- or point these variables somewhere else. A host with neither the schemas nor
# xmllint gets no test, rather than a test that silently passes.
#
# Included before the test directories so both libs/etg/tests and libs/node/tests can use it.

find_program(XMLLINT_EXECUTABLE xmllint)
set(VALIDATE_XML_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/validate_xml.cmake")
set(MM_ENI_SCHEMA
    "${CMAKE_SOURCE_DIR}/libs/etg/tests/data/eni/EtherCATConfig.xsd"
    CACHE FILEPATH "ENI XML Schema (EtherCATConfig.xsd), for the ENI validation tests")
set(MM_ESI_SCHEMA
    "${CMAKE_SOURCE_DIR}/libs/etg/tests/data/esi/EtherCATInfo.xsd"
    CACHE FILEPATH "ESI XML Schema (EtherCATInfo.xsd), for the ESI validation test")

if(NOT XMLLINT_EXECUTABLE)
  message(STATUS "Schema validation tests disabled: xmllint was not found")
endif()

# Adds a test that generates a document with one GoogleTest case and validates it, or -- with no
# TESTS/FILTER -- that validates a document already on disk.
function(mm_add_schema_validation_test)
  cmake_parse_arguments(ARG "" "NAME;SCHEMA;DOCUMENT;TESTS;FILTER" "" ${ARGN})
  if(NOT XMLLINT_EXECUTABLE)
    return()
  endif()
  if(NOT EXISTS "${ARG_SCHEMA}")
    message(STATUS "${ARG_NAME} disabled: ${ARG_SCHEMA} is not present")
    return()
  endif()
  add_test(
    NAME ${ARG_NAME}
    COMMAND
      ${CMAKE_COMMAND} -DXMLLINT=${XMLLINT_EXECUTABLE} -DSCHEMA=${ARG_SCHEMA}
      -DDOCUMENT=${ARG_DOCUMENT} -DTESTS=${ARG_TESTS} -DFILTER=${ARG_FILTER} -P
      ${VALIDATE_XML_SCRIPT})
endfunction()
