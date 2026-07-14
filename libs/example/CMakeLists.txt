add_library(example STATIC example_logic.cc example_routes.cc)
add_library(mm::example ALIAS example)

target_include_directories(example PUBLIC ${CMAKE_SOURCE_DIR}/libs)

target_link_libraries(
  example PUBLIC mm::node # domain access: DeviceManager / Device
                 mm::api # HTTP-transport glue: RouteContext + sendJson (pulls in uWS + nlohmann)
)
