add_executable(motion_master
    main.cc
)

set_target_properties(motion_master PROPERTIES OUTPUT_NAME "motion-master")

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(motion_master PRIVATE
    mm_comm
    CLI11::CLI11
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)
