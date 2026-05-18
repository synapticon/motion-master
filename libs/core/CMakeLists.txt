find_package(semver CONFIG REQUIRED)

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")

if(WIN32)
  set(CYCLIC_TIMER_SRC cyclic_timer_windows.cc)
else()
  set(CYCLIC_TIMER_SRC cyclic_timer_linux.cc)
endif()

add_library(mm_core STATIC
    version.cc
    ${CYCLIC_TIMER_SRC}
)

target_include_directories(mm_core PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(mm_core PUBLIC semver::semver)

add_subdirectory(tests)
