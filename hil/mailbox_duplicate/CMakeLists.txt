if(NOT WIN32)
  add_executable(mailbox_duplicate main.cc)
  target_link_libraries(mailbox_duplicate PRIVATE mm::comm)
endif()
