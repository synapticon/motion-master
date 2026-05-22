add_library(node STATIC
    device.cc
    device_manager.cc
)
add_library(mm::node ALIAS node)

target_include_directories(node PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(node PUBLIC
    mm::comm
)
