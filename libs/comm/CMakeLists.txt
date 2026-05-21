add_library(comm STATIC
    fieldbus_driver.cc
    soem_fieldbus_driver.cc
    soem_util.cc
)
add_library(mm::comm ALIAS comm)

target_include_directories(comm PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

find_package(soem CONFIG REQUIRED)

target_link_libraries(comm PUBLIC
    mm::core
    soem
)
