add_executable(motion_master
    main.cc
    options.cc
    http_server.cc
    ws_server.cc
    game_loop.cc
    monitoring_api.cc
    cert_info.cc
    cert_updater.cc
)

set_target_properties(motion_master PROPERTIES OUTPUT_NAME "motion-master")

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

target_link_libraries(motion_master PRIVATE
    mm::node
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    unofficial::uwebsockets::uwebsockets
    OpenSSL::Crypto
    CURL::libcurl
)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(motion_master PRIVATE Threads::Threads)

endif()

add_subdirectory(tests)
