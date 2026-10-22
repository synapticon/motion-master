if(NOT WIN32)
  add_executable(foe_read_abort main.cc)
  target_link_libraries(foe_read_abort PRIVATE mm::comm)
endif()
