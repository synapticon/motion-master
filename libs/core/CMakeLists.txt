find_package(semver CONFIG REQUIRED)

configure_file(version.hh.in "${CMAKE_CURRENT_BINARY_DIR}/version.hh")

add_library(mm_core STATIC
    version.cc
)

target_include_directories(mm_core PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(mm_core PUBLIC semver::semver)

add_subdirectory(tests)
