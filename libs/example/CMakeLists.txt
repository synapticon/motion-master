add_library(example STATIC example_cyclic_task.cc example_logic.cc example_routes.cc)
add_library(mm::example ALIAS example)

target_include_directories(example PUBLIC ${CMAKE_SOURCE_DIR}/libs)

# Dependencies:
#
# * mm::node: domain access (DeviceManager / Device)
# * mm::api: HTTP-transport glue (RouteContext + sendJson; pulls in uWS + nlohmann)
#
# The Tier-3 starter (example_cyclic_task) needs only mm::node — CyclicTask lives in mm::core,
# which mm::node already brings in.
target_link_libraries(example PUBLIC mm::node mm::api)
