# Header-only HTTP-transport glue (RouteContext, RegisterRoutesFn, sendJson/Error/Status). This is
# the only layer that knows about uWebSockets; mm::node stays transport-agnostic. INTERFACE because
# there is no .cc to compile — it just propagates its public header plus the uWS/json include dirs.
add_library(api INTERFACE)
add_library(mm::api ALIAS api)

target_include_directories(api INTERFACE ${CMAKE_SOURCE_DIR}/libs)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)

target_link_libraries(api INTERFACE nlohmann_json::nlohmann_json
                                    unofficial::uwebsockets::uwebsockets)
