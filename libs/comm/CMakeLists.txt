add_library(comm STATIC
    al_status_codes.cc
    base.cc
    esc_registers.cc
    fieldbus_driver.cc
    foe_error_codes.cc
    object_data_types.cc
    soem_fieldbus_driver.cc
)
add_library(mm::comm ALIAS comm)

target_include_directories(comm PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(soem CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(comm PUBLIC
    mm::core
    nlohmann_json::nlohmann_json
    soem
    spdlog::spdlog
)

# Windows: SOEM's NIC driver builds against Npcap. Pull in <pcap.h> and the
# wpcap/Packet import libs via the shared mm::npcap target (see root CMakeLists).
# PUBLIC so consumers of <soem/soem.h> and the final executables inherit both.
if(WIN32)
  target_link_libraries(comm PUBLIC mm::npcap)
endif()
