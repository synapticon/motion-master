add_library(
  etg STATIC
  esi.cc
  esi_data_type.cc
  esi_entry.cc
  esi_request.cc
  esi_unit.cc
  fsoe_crc.cc
  fsoe_frame.cc
  fsoe_master.cc
  safety_drive_profile.cc)
add_library(mm::etg ALIAS etg)

target_include_directories(etg PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(pugixml CONFIG REQUIRED)

target_link_libraries(etg PUBLIC mm::core nlohmann_json::nlohmann_json)

# pugixml is PRIVATE on purpose: it appears only in esi.cc and no pugi:: type reaches a public
# header, so consumers of mm::etg do not inherit an XML parser in their include path.
target_link_libraries(etg PRIVATE pugixml::pugixml)

add_subdirectory(tests)
