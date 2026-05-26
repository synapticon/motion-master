add_library(comm STATIC
    al_status_codes.cc
    base.cc
    esc_registers.cc
    fieldbus_driver.cc
    soem_fieldbus_driver.cc
)
add_library(mm::comm ALIAS comm)

target_include_directories(comm PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(soem CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(comm PUBLIC
    mm::core
    nlohmann_json::nlohmann_json
    soem
    spdlog::spdlog
)
