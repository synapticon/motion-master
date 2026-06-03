# This software is dual-licensed under GPLv3 and a commercial
# license. See the file LICENSE.md distributed with this software for
# full license information.
#
# Darwin (macOS) platform configuration for SOEM 2.0.
#
# Provided by the motion-master vcpkg overlay port (ports/soem): upstream SOEM
# ships only Linux.cmake / Windows.cmake / rt-kernel.cmake.  The osal/macosx
# layer is maintained in the overlay; the oshw/macosx layer (BPF/libpcap NIC
# driver) is upstream's contrib copy, already written against the 2.0 API.
# Both are copied into the source tree by portfile.cmake before configure.

target_sources(soem PRIVATE
  osal/macosx/osal.c
  osal/macosx/osal_defs.h
  oshw/macosx/oshw.c
  oshw/macosx/oshw.h
  oshw/macosx/nicdrv.c
  oshw/macosx/nicdrv.h
)

target_include_directories(soem PUBLIC
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/osal/macosx>
  $<BUILD_INTERFACE:${SOEM_SOURCE_DIR}/oshw/macosx>
  $<INSTALL_INTERFACE:include/soem>
)

foreach(target IN ITEMS
    soem
    ec_sample
    eepromtool
    eni_test
    firm_update
    simple_ng
    slaveinfo)
  if (TARGET ${target})
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
    )
  endif()
endforeach()

# libpcap ships with macOS (SDK headers + /usr/lib/libpcap.dylib); no vcpkg dep.
target_link_libraries(soem PUBLIC pcap pthread)

install(FILES
  osal/macosx/osal_defs.h
  oshw/macosx/nicdrv.h
  DESTINATION include/soem
)
