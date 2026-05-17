add_library(mm_comm STATIC
    fieldbus_driver.cc
)

target_include_directories(mm_comm PUBLIC
    ${CMAKE_SOURCE_DIR}/libs
)

target_link_libraries(mm_comm PUBLIC mm_core)
