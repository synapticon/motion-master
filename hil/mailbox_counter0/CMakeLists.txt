if(NOT WIN32)
  add_executable(mailbox_counter0 main.cc)
  target_link_libraries(mailbox_counter0 PRIVATE mm::comm)
endif()
