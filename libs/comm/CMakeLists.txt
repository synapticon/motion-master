add_library(
  comm STATIC
  al_status_codes.cc
  base.cc
  esc_diagnostics.cc
  esc_registers.cc
  fieldbus_driver.cc
  foe_error_codes.cc
  mailbox_error_codes.cc
  object_data_types.cc
  sdo_abort_codes.cc
  sii.cc
  soem_fieldbus_driver.cc
  spoe_connection.cc
  spoe_fieldbus_driver.cc
  spoe_frame.cc
  spoe_process_data.cc)
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

# The IgH EtherCAT Master driver. Off by default, because it needs the IgH kernel module on the host
# and its source tree at build time. The driver includes master/ioctl.h, which IgH does not install.
option(MM_WITH_IGH "Build the IgH EtherCAT Master fieldbus driver (Linux only)" OFF)
if(MM_WITH_IGH)
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "MM_WITH_IGH needs Linux")
  endif()
  set(MM_IGH_SOURCE_DIR
      ""
      CACHE PATH "IgH EtherCAT Master source tree, of the same version as the loaded module")
  set(MM_IGH_MAX_NUM_DEVICES
      1
      CACHE STRING "The --with-devices option the IgH module was configured with")
  if(NOT EXISTS "${MM_IGH_SOURCE_DIR}/master/ioctl.h")
    message(FATAL_ERROR "MM_IGH_SOURCE_DIR must name an IgH source tree that holds master/ioctl.h")
  endif()
  # The user-space library of the same IgH version. It is LGPL, so it is linked dynamically.
  find_library(MM_IGH_LIBRARY ethercat REQUIRED)
  configure_file(igh_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/igh/config.h @ONLY)
  target_sources(comm PRIVATE igh_fieldbus_driver.cc)
  # SYSTEM, because the IgH headers are C and are not held to the warning flags of this project.
  target_include_directories(comm SYSTEM PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/igh
                                                 ${MM_IGH_SOURCE_DIR})
  target_link_libraries(comm PRIVATE ${MM_IGH_LIBRARY})
  target_compile_definitions(comm PUBLIC MM_WITH_IGH)
endif()

add_subdirectory(tests)
