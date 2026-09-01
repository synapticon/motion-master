# Validates the generated ENI document against the ENI XML Schema.
#
# Two steps run here rather than one, because the document does not exist until the writer has run
# and CTest cannot order a GoogleTest case ahead of an external command. pugixml does not validate
# against a schema, so xmllint is what checks conformance.
#
# Driven from tests/CMakeLists.txt with TESTS, XMLLINT, SCHEMA and DOCUMENT set.

execute_process(
  COMMAND "${TESTS}" "--gtest_filter=EniWriterTest.WritesTheReferenceDocumentForSchemaValidation"
  RESULT_VARIABLE write_result
  OUTPUT_VARIABLE write_output
  ERROR_VARIABLE write_output)

if(NOT write_result EQUAL 0)
  message(FATAL_ERROR "writing the reference ENI failed:\n${write_output}")
endif()

execute_process(
  COMMAND "${XMLLINT}" --noout --schema "${SCHEMA}" "${DOCUMENT}"
  RESULT_VARIABLE lint_result
  OUTPUT_VARIABLE lint_stdout
  ERROR_VARIABLE lint_output)

if(NOT lint_result EQUAL 0)
  message(FATAL_ERROR "${DOCUMENT} does not validate against ${SCHEMA}:\n${lint_output}")
endif()
