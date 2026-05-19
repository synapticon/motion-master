find_package(soem CONFIG REQUIRED)

add_executable(playground
    main.cc
)

target_link_libraries(playground PRIVATE soem)
