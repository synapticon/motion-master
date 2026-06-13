find_package(semver CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")

if(WIN32)
  set(CYCLIC_TIMER_SRC cyclic_timer_windows.cc)
elseif(APPLE)
  set(CYCLIC_TIMER_SRC cyclic_timer_macos.cc)
else()
  set(CYCLIC_TIMER_SRC cyclic_timer_linux.cc)
endif()

add_library(core STATIC
    version.cc
    system_info.cc
    ${CYCLIC_TIMER_SRC}
)
add_library(mm::core ALIAS core)

target_include_directories(core PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
    ${CMAKE_CURRENT_BINARY_DIR}/..
)

target_link_libraries(core PUBLIC semver::semver nlohmann_json::nlohmann_json)

add_subdirectory(tests)
