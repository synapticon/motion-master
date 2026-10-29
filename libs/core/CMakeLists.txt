find_package(semver CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)

configure_file(version.h.in "${CMAKE_CURRENT_BINARY_DIR}/version.h")

if(WIN32)
  set(CYCLIC_TIMER_SRC cyclic_timer_windows.cc)
  set(CHILD_PROCESS_SRC child_process_windows.cc)
elseif(APPLE)
  set(CYCLIC_TIMER_SRC cyclic_timer_macos.cc)
  set(CHILD_PROCESS_SRC child_process_posix.cc)
else()
  set(CYCLIC_TIMER_SRC cyclic_timer_linux.cc)
  set(CHILD_PROCESS_SRC child_process_posix.cc)
endif()

add_library(
  core STATIC
  base64.cc
  version.cc
  system_info.cc
  platform.cc
  realtime.cc
  user_cache.cc
  ${CYCLIC_TIMER_SRC}
  ${CHILD_PROCESS_SRC})
add_library(mm::core ALIAS core)

target_include_directories(core PUBLIC ${CMAKE_SOURCE_DIR}/libs ${CMAKE_CURRENT_BINARY_DIR}/..)

# OpenSSL is an implementation detail of base64.cc — no OpenSSL type appears in a core header — but
# a static library still propagates the link requirement to whoever links core.
target_link_libraries(
  core
  PUBLIC semver::semver nlohmann_json::nlohmann_json
  PRIVATE OpenSSL::Crypto)

add_subdirectory(tests)

# SetProcessInformation and the power-throttling state need Windows 8 or newer. 0x0A00 is Windows
# 10, the same target as comm.
if(WIN32)
  target_compile_definitions(core PRIVATE _WIN32_WINNT=0x0A00)
endif()
