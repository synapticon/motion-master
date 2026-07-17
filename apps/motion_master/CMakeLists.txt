# Embed swagger.yml into the binary (served verbatim at GET /api/swagger.yml) so a client can fetch
# the running server's exact contract with no packaged file. Regenerated whenever the spec changes;
# byte-array embedding (see cmake/embed_file.cmake) round-trips arbitrary content safely.
set(SWAGGER_SPEC_H "${CMAKE_CURRENT_BINARY_DIR}/swagger_spec.h")
add_custom_command(
  OUTPUT "${SWAGGER_SPEC_H}"
  COMMAND ${CMAKE_COMMAND} -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/swagger.yml
          -DOUTPUT=${SWAGGER_SPEC_H} -DVAR=kSwaggerYml -P ${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/swagger.yml" "${CMAKE_SOURCE_DIR}/cmake/embed_file.cmake"
  COMMENT "Embedding swagger.yml"
  VERBATIM)

add_executable(
  motion_master
  main.cc
  options.cc
  config.cc
  http_server.cc
  ws_server.cc
  game_loop.cc
  monitoring_api.cc
  cert_info.cc
  cert_updater.cc
  "${SWAGGER_SPEC_H}")

# The generated swagger_spec.h lives in the binary dir; http_server.cc includes it from there.
target_include_directories(motion_master PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

set_target_properties(motion_master PROPERTIES OUTPUT_NAME "motion-master")

find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(unofficial-uwebsockets CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)

target_link_libraries(
  motion_master
  PRIVATE mm::node
          mm::example
          CLI11::CLI11
          nlohmann_json::nlohmann_json
          spdlog::spdlog
          unofficial::uwebsockets::uwebsockets
          OpenSSL::Crypto
          CURL::libcurl)

if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(motion_master PRIVATE Threads::Threads)
endif()

add_subdirectory(tests)
