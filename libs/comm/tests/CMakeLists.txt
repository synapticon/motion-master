find_package(asio CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)

# The fake SPoE server is a library, because the node tests drive a whole DeviceManager against it.
add_library(mm_comm_spoe_fake STATIC fake_spoe_server.cc)
add_library(mm::comm_spoe_fake ALIAS mm_comm_spoe_fake)
target_include_directories(mm_comm_spoe_fake PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mm_comm_spoe_fake PRIVATE asio::asio)
if(WIN32)
  target_compile_definitions(mm_comm_spoe_fake PRIVATE _WIN32_WINNT=0x0A00)
endif()

add_executable(
  mm_comm_tests
  base_test.cc
  fake_spoe_server_test.cc
  sii_test.cc
  soem_fieldbus_driver_test.cc
  spoe_connection_test.cc
  spoe_fieldbus_driver_test.cc
  spoe_process_data_test.cc
  spoe_frame_test.cc
  vendor_ids_test.cc)

target_link_libraries(mm_comm_tests PRIVATE mm::comm mm::comm_spoe_fake asio::asio GTest::gtest
                                            GTest::gtest_main)

# Asio needs the oldest Windows version to target, and warns when it is not set. 0x0A00 is Windows
# 10.
if(WIN32)
  target_compile_definitions(mm_comm_tests PRIVATE _WIN32_WINNT=0x0A00)
endif()

# Constructing a SoemFieldbusDriver pulls SOEM's win32 NIC driver into the link, so this executable
# imports wpcap.dll — which ships with the Npcap driver, not with the vendored SDK we build against.
# The Windows loader resolves imports before main, so on a machine without Npcap the exe cannot even
# start (STATUS_DLL_NOT_FOUND), and gtest_discover_tests runs it at *build* time: the build fails
# rather than a test. Delay-loading defers that resolution to the first pcap call, which no test
# here makes — only init() opens a NIC, and none of them calls it. That is what lets these tests run
# on a machine with no EtherCAT NIC and no Npcap installed, CI included.
if(MSVC)
  target_link_options(mm_comm_tests PRIVATE /DELAYLOAD:wpcap.dll /DELAYLOAD:Packet.dll)
  target_link_libraries(mm_comm_tests PRIVATE delayimp.lib)
endif()

include(GoogleTest)
gtest_discover_tests(mm_comm_tests DISCOVERY_TIMEOUT 60)
