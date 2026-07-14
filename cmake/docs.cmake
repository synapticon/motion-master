find_package(Doxygen OPTIONAL_COMPONENTS dot)

if(DOXYGEN_FOUND)
  configure_file("${CMAKE_SOURCE_DIR}/Doxyfile.in" "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile" @ONLY)

  add_custom_target(
    docs
    COMMAND ${DOXYGEN_EXECUTABLE} "${CMAKE_CURRENT_BINARY_DIR}/Doxyfile"
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Generating API documentation with Doxygen"
    VERBATIM)
else()
  add_custom_target(
    docs
    COMMAND ${CMAKE_COMMAND} -E echo
            "Doxygen not found -- install with: sudo dnf install doxygen  # or apt/brew"
    VERBATIM)
  message(STATUS "Doxygen not found -- 'docs' target will print an install hint")
endif()
