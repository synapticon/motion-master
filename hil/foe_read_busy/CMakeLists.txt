if(NOT WIN32)
  add_executable(foe_read_busy main.cc)
  target_link_libraries(foe_read_busy PRIVATE mm::comm)
endif()
