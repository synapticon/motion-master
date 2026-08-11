find_package(GTest CONFIG REQUIRED)

add_executable(mm_comm_tests base_test.cc sii_test.cc soem_fieldbus_driver_test.cc)

target_link_libraries(mm_comm_tests PRIVATE mm::comm GTest::gtest GTest::gtest_main)

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
