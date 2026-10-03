add_library(
  comm STATIC
  al_status_codes.cc
  base.cc
  esc_registers.cc
  fieldbus_driver.cc
  foe_error_codes.cc
  mailbox_error_codes.cc
  object_data_types.cc
  sdo_abort_codes.cc
  sii.cc
  soem_fieldbus_driver.cc
  spoe_connection.cc
  spoe_frame.cc)
add_library(mm::comm ALIAS comm)

target_include_directories(comm PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(asio CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(soem CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(comm PUBLIC mm::core nlohmann_json::nlohmann_json soem spdlog::spdlog)

# The SPoE connection uses asio in its implementation file only, so no public header includes it.
target_link_libraries(comm PRIVATE asio::asio)

# Asio needs the oldest Windows version to target, and warns when it is not set. 0x0A00 is Windows
# 10.
if(WIN32)
  target_compile_definitions(comm PRIVATE _WIN32_WINNT=0x0A00)
endif()

# Windows: SOEM's NIC driver builds against Npcap. Pull in <pcap.h> and the wpcap/Packet import libs
# via the shared mm::npcap target (see root CMakeLists). PUBLIC so consumers of <soem/soem.h> and
# the final executables inherit both.
if(WIN32)
  target_link_libraries(comm PUBLIC mm::npcap)
endif()

add_subdirectory(tests)
