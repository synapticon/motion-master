# Reads EtherCAT frames back from a capture and says how late each one came back. Pure: a transform
# over timestamped bytes, with no socket and no SOEM, so the summary runs over a pcap from any host
# and the tests run without a bus. The program in apps/capture does the capturing.
find_package(nlohmann_json CONFIG REQUIRED)

add_library(capture STATIC ethercat_frame.cc pcap.cc summary.cc)
add_library(mm::capture ALIAS capture)

target_include_directories(capture PUBLIC ${CMAKE_SOURCE_DIR}/libs)

target_link_libraries(capture PUBLIC mm::core nlohmann_json::nlohmann_json)

add_subdirectory(tests)
