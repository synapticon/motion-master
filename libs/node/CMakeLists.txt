add_library(
  node STATIC
  cia402_control.cc
  cia402_drive.cc
  device.cc
  device_manager.cc
  device_parameter.cc
  monitoring.cc
  monitoring_manager.cc
  parameter_cache.cc
  parameter_refresher.cc
  pdo_mapping.cc
  procedure.cc
  procedure_manager.cc
  profile_control.cc
  profile_device.cc
  process_data_dump.cc
  process_data_ring.cc
  process_image.cc
  somanet_drive.cc)
add_library(mm::node ALIAS node)

target_include_directories(node PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(node PUBLIC mm::comm nlohmann_json::nlohmann_json spdlog::spdlog)

add_subdirectory(tests)
