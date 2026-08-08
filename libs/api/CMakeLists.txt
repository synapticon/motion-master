# HTTP-transport glue: the Router (handlers that run off the event loop), RouteContext,
# RegisterRoutesFn and the sendJson/Error/Status helpers. This is the only layer that knows about
# uWebSockets; mm::node stays transport-agnostic.
add_library(api STATIC router.cc)
add_library(mm::api ALIAS api)

target_include_directories(api PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
# spdlog here is built against an external fmt, and mm_api_tests links mm::api without the comm/node
# chain that would otherwise drag fmt in — so name it explicitly.
find_package(fmt CONFIG REQUIRED)

target_link_libraries(api PUBLIC nlohmann_json::nlohmann_json unofficial::uwebsockets::uwebsockets
                                 spdlog::spdlog fmt::fmt)

add_subdirectory(tests)
