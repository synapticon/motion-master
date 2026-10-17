if(NOT WIN32)
  add_executable(foe_abort main.cc)
  target_link_libraries(foe_abort PRIVATE mm::comm)
endif()
