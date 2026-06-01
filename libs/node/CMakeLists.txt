add_library(node STATIC
    device.cc
    device_manager.cc
    device_parameter.cc
    process_image.cc
)
add_library(mm::node ALIAS node)

target_include_directories(node PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(node PUBLIC
    mm::comm
    nlohmann_json::nlohmann_json
    spdlog::spdlog
)

add_subdirectory(tests)
