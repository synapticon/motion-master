file(GLOB_RECURSE PROJECT_SOURCE_FILES
    "${CMAKE_SOURCE_DIR}/apps/*.cc"
    "${CMAKE_SOURCE_DIR}/apps/*.h"
    "${CMAKE_SOURCE_DIR}/libs/*.cc"
    "${CMAKE_SOURCE_DIR}/libs/*.h"
)

# ---------- cpplint ----------
find_program(CPPLINT_EXECUTABLE cpplint)

if(CPPLINT_EXECUTABLE)
    add_custom_target(lint
        COMMAND ${CPPLINT_EXECUTABLE} ${PROJECT_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running cpplint"
        VERBATIM
    )
else()
    add_custom_target(lint
        COMMAND ${CMAKE_COMMAND} -E echo
            "cpplint not found -- install with: pip install cpplint"
        VERBATIM
    )
    message(STATUS "cpplint not found -- 'lint' target will print an install hint")
endif()

# ---------- clang-format ----------
find_program(CLANG_FORMAT_EXECUTABLE clang-format)

if(CLANG_FORMAT_EXECUTABLE)
    add_custom_target(format
        COMMAND ${CLANG_FORMAT_EXECUTABLE} -i ${PROJECT_SOURCE_FILES}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        COMMENT "Running clang-format"
        VERBATIM
    )
else()
    add_custom_target(format
        COMMAND ${CMAKE_COMMAND} -E echo "clang-format not found"
        VERBATIM
    )
    message(STATUS "clang-format not found -- 'format' target unavailable")
endif()

# ---------- cppcheck ----------
find_program(CPPCHECK_EXECUTABLE cppcheck)

if(CPPCHECK_EXECUTABLE)
    add_custom_target(cppcheck
        COMMAND ${CPPCHECK_EXECUTABLE}
            --enable=warning,style,performance,portability
            --std=c++23
            --suppress=missingIncludeSystem
            --suppressions-list=${CMAKE_SOURCE_DIR}/cppcheck.supp
            --error-exitcode=1
            --quiet
            -I ${CMAKE_SOURCE_DIR}/libs
            ${CMAKE_SOURCE_DIR}/apps
            ${CMAKE_SOURCE_DIR}/libs
        COMMENT "Running cppcheck"
        VERBATIM
    )
else()
    add_custom_target(cppcheck
        COMMAND ${CMAKE_COMMAND} -E echo "cppcheck not found"
        VERBATIM
    )
    message(STATUS "cppcheck not found -- 'cppcheck' target unavailable")
endif()
