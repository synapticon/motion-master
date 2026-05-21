if(NOT WIN32)
  add_executable(jitter_bench main.cc)

  find_package(Threads REQUIRED)
  target_link_libraries(jitter_bench PRIVATE mm::core Threads::Threads)
endif()
