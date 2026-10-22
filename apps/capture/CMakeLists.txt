# motion-master-capture: a one-shot program that records every EtherCAT frame on an adapter for a
# chosen number of seconds, with kernel timestamps, and writes a pcap file and a summary of how late
# each frame came back. It ships beside motion-master and runs by hand or from the Console. The
# summary is mm::capture; this is the socket and the command line.
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

if(WIN32)
  set(CAPTURE_PLATFORM_SRC capture_pcap.cc)
elseif(APPLE)
  set(CAPTURE_PLATFORM_SRC capture_pcap.cc)
else()
  set(CAPTURE_PLATFORM_SRC capture_linux.cc)
endif()

add_executable(motion_master_capture main.cc ${CAPTURE_PLATFORM_SRC})
set_target_properties(motion_master_capture PROPERTIES OUTPUT_NAME "motion-master-capture")

target_link_libraries(motion_master_capture PRIVATE mm::capture mm::core CLI11::CLI11
                                                    nlohmann_json::nlohmann_json)

# Linux captures through a raw socket and needs no library. Windows and macOS capture through pcap:
# the Npcap SDK the build vendors for SOEM, and the libpcap macOS ships.
if(WIN32)
  target_link_libraries(motion_master_capture PRIVATE mm::npcap)
elseif(APPLE)
  find_library(PCAP_LIBRARY pcap REQUIRED)
  target_link_libraries(motion_master_capture PRIVATE ${PCAP_LIBRARY})
endif()

# Lands beside motion-master, which is where the server looks for it and where the packages take it
# from.
set_target_properties(motion_master_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY
                                                       "${CMAKE_BINARY_DIR}/apps/motion_master")
