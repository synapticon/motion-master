# The auto-tuning child process and the calls made to it. It is a library rather than part of the
# application because a procedure will drive a tuning sequence from mm::node: run a measurement on
# the drive, hand the samples to auto-tuning, write the gains back. mm::node cannot depend on the
# application, so this has to sit below both.
find_package(nlohmann_json CONFIG REQUIRED)

if(WIN32)
  set(AUTO_TUNING_PLATFORM_SRC process_windows.cc)
else()
  set(AUTO_TUNING_PLATFORM_SRC process_posix.cc)
endif()

add_library(auto_tuning STATIC process.cc ${AUTO_TUNING_PLATFORM_SRC})
add_library(mm::auto_tuning ALIAS auto_tuning)

target_include_directories(auto_tuning PUBLIC ${CMAKE_SOURCE_DIR}/libs)

# mm::net is an implementation detail: it appears in no header here. mm::core is used for its
# platform helpers. nlohmann is PUBLIC because the tuning calls exchange JSON documents.
target_link_libraries(
  auto_tuning
  PUBLIC nlohmann_json::nlohmann_json
  PRIVATE mm::net mm::core)

add_subdirectory(tests)
