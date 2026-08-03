file(GLOB_RECURSE PROJECT_SOURCE_FILES "${CMAKE_SOURCE_DIR}/apps/*.cc"
     "${CMAKE_SOURCE_DIR}/apps/*.h" "${CMAKE_SOURCE_DIR}/libs/*.cc" "${CMAKE_SOURCE_DIR}/libs/*.h")

# ---------- cpplint ----------
find_program(CPPLINT_EXECUTABLE cpplint)

if(CPPLINT_EXECUTABLE)
  add_custom_target(
    lint
    COMMAND ${CPPLINT_EXECUTABLE} ${PROJECT_SOURCE_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running cpplint"
    VERBATIM)
else()
  add_custom_target(
    lint
    COMMAND ${CMAKE_COMMAND} -E echo "cpplint not found -- install with: pip install cpplint"
    COMMENT "cpplint unavailable -- printing install hint"
    VERBATIM)
  message(STATUS "cpplint not found -- 'lint' target will print an install hint")
endif()

# ---------- clang-format ----------
find_program(CLANG_FORMAT_EXECUTABLE clang-format)

if(CLANG_FORMAT_EXECUTABLE)
  add_custom_target(
    format
    COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${PROJECT_SOURCE_FILES}
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Running clang-format"
    VERBATIM)
else()
  add_custom_target(
    format
    COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
    COMMENT "clang-format unavailable -- printing install hint"
    VERBATIM)
  message(STATUS "clang-format not found -- 'format' target unavailable")
endif()

# ---------- cppcheck ----------
find_program(CPPCHECK_EXECUTABLE cppcheck)

# cppcheck's findings change between releases, so .cppcheck-version pins one version for CI and
# every developer machine alike. This is not pedantry: 2.13 reports a scope guard's null check as
# always-true where 2.21 correctly does not, so a suppression list is only valid for the version it
# was written against, and a local check that passes while CI fails is worse than no local check. CI
# builds the pinned version from source (tools/install-cppcheck.sh) because no distribution offers a
# choice; run that script if your system's cppcheck is not the pinned one.
#
# The mismatch is detected here but reported by the target, not at configure time: it is only worth
# saying when someone actually runs the check, and a warning on every unrelated cmake configure
# would soon be scrolled past. It warns rather than fails, because a mismatched cppcheck is still
# useful — it just is not the one whose verdict gates the build.
set(CPPCHECK_VERSION_NOTE "")
if(CPPCHECK_EXECUTABLE)
  file(STRINGS "${CMAKE_SOURCE_DIR}/.cppcheck-version" CPPCHECK_PINNED_VERSION)
  execute_process(
    COMMAND ${CPPCHECK_EXECUTABLE} --version
    OUTPUT_VARIABLE CPPCHECK_VERSION_OUTPUT
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  string(REGEX MATCH "[0-9]+\\.[0-9]+(\\.[0-9]+)?" CPPCHECK_LOCAL_VERSION
               "${CPPCHECK_VERSION_OUTPUT}")
  if(NOT CPPCHECK_LOCAL_VERSION VERSION_EQUAL CPPCHECK_PINNED_VERSION)
    set(CPPCHECK_VERSION_NOTE
        "NOTE: local cppcheck is ${CPPCHECK_LOCAL_VERSION}, CI runs ${CPPCHECK_PINNED_VERSION} (.cppcheck-version) -- findings differ between versions, so a clean run here does not guarantee a clean CI run."
    )
  endif()
endif()

if(CPPCHECK_EXECUTABLE)
  # The version note (if any) is echoed first, so it is read before the findings rather than after.
  if(CPPCHECK_VERSION_NOTE)
    set(CPPCHECK_NOTE_COMMAND COMMAND ${CMAKE_COMMAND} -E echo "${CPPCHECK_VERSION_NOTE}")
  else()
    set(CPPCHECK_NOTE_COMMAND "")
  endif()
  add_custom_target(
    cppcheck
    ${CPPCHECK_NOTE_COMMAND}
    COMMAND
      ${CPPCHECK_EXECUTABLE} --enable=warning,style,performance,portability --std=c++23
      --suppress=missingIncludeSystem --suppressions-list=${CMAKE_SOURCE_DIR}/cppcheck.supp
      --error-exitcode=1 --quiet -I ${CMAKE_SOURCE_DIR}/libs ${CMAKE_SOURCE_DIR}/apps
      ${CMAKE_SOURCE_DIR}/libs
    COMMENT "Running cppcheck"
    VERBATIM)
else()
  add_custom_target(
    cppcheck
    COMMAND ${CMAKE_COMMAND} -E echo "cppcheck not found"
    COMMENT "cppcheck unavailable -- printing install hint"
    VERBATIM)
  message(STATUS "cppcheck not found -- 'cppcheck' target unavailable")
endif()
