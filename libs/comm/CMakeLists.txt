add_library(comm STATIC
    base.cc
    fieldbus_driver.cc
    soem_fieldbus_driver.cc
)
add_library(mm::comm ALIAS comm)

target_include_directories(comm PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

find_package(soem CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

target_link_libraries(comm PUBLIC
    mm::core
    soem
    spdlog::spdlog
)
