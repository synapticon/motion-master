add_library(
  node STATIC
  cia402_control.cc
  cia402_drive.cc
  device.cc
  device_manager.cc
  device_parameter.cc
  eni_collector.cc
  firmware_package.cc
  hardware_description.cc
  integro_variant.cc
  ic_haus_registers.cc
  kuebler_registers.cc
  monitoring.cc
  notification_bus.cc
  monitoring_manager.cc
  operation_modes.cc
  parameter_cache.cc
  parameter_refresher.cc
  pdo_mapping.cc
  procedure.cc
  procedure_catalogue.cc
  procedure_manager.cc
  profile_device.cc
  profile_procedures.cc
  process_data_dump.cc
  process_data_ring.cc
  process_image.cc
  somanet_control.cc
  somanet_drive.cc
  somanet_procedures.cc)
add_library(mm::node ALIAS node)

target_include_directories(node PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(libzip CONFIG REQUIRED)

# libzip is an implementation detail of firmware_package.cc — no libzip type appears in a node
# header — but a static library still propagates the link requirement to whoever links node. mm::etg
# is PUBLIC because eni_collector.h names its types: the collector fills an mm::etg::EniNetwork,
# which the caller then hands to mm::etg::writeEni.
target_link_libraries(
  node
  PUBLIC mm::comm mm::etg nlohmann_json::nlohmann_json spdlog::spdlog
  PRIVATE libzip::zip)

add_subdirectory(tests)
