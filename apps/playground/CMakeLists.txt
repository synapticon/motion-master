find_package(soem CONFIG REQUIRED)

add_executable(playground
    main.cc
)

target_link_libraries(playground PRIVATE soem)

# Windows: soem's win32 NIC driver needs Npcap headers + import libs.
if(WIN32)
  target_link_libraries(playground PRIVATE mm::npcap)
endif()
