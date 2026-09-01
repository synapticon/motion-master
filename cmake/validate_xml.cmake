# Validates an XML document against an XML Schema with xmllint.
#
# pugixml does not validate against a schema, so xmllint is what checks conformance: for the ENI
# this library writes, and for the vendor ESI it parses. Driven from tests/CMakeLists.txt with
# XMLLINT, SCHEMA and DOCUMENT set, plus TESTS and FILTER when the document is generated rather than
# checked in. Generating runs here rather than in a second test, because the document does not exist
# until the writer has run and CTest cannot order a GoogleTest case ahead of a command.

if(TESTS)
  execute_process(
    COMMAND "${TESTS}" "--gtest_filter=${FILTER}"
    RESULT_VARIABLE GENERATE_RESULT
    OUTPUT_VARIABLE GENERATE_OUTPUT
    ERROR_VARIABLE GENERATE_OUTPUT)
  if(NOT GENERATE_RESULT EQUAL 0)
    message(FATAL_ERROR "writing ${DOCUMENT} failed:\n${GENERATE_OUTPUT}")
  endif()
endif()

execute_process(
  COMMAND "${XMLLINT}" --noout --schema "${SCHEMA}" "${DOCUMENT}"
  RESULT_VARIABLE LINT_RESULT
  OUTPUT_VARIABLE LINT_STDOUT
  ERROR_VARIABLE LINT_OUTPUT)

if(NOT LINT_RESULT EQUAL 0)
  message(FATAL_ERROR "${DOCUMENT} does not validate against ${SCHEMA}:\n${LINT_OUTPUT}")
endif()
