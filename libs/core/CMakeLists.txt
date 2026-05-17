find_package(semver CONFIG REQUIRED)

add_library(mm_core STATIC
    version.cc
)

target_include_directories(mm_core PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(mm_core PUBLIC semver::semver)

add_subdirectory(tests)
