# Copies swagger.yml into the build output directory so the development layout
# matches production — both have swagger.yml next to the binary.
configure_file(swagger.yml swagger.yml COPYONLY)

add_executable(motion_master
    main.cc
    options.cc
    server.cc
    game_loop.cc
)

set_target_properties(motion_master PROPERTIES OUTPUT_NAME "motion-master")

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)

target_link_libraries(motion_master PRIVATE
    mm::node
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    unofficial::uwebsockets::uwebsockets
)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(motion_master PRIVATE Threads::Threads)

endif()

add_subdirectory(tests)
