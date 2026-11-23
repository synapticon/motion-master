if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(ethernet_discovery main.cc)
  target_link_libraries(ethernet_discovery PRIVATE mm::core)
endif()
