if(NOT WIN32)
  add_executable(held_frame main.cc)
  target_link_libraries(held_frame PRIVATE mm::comm)
endif()
