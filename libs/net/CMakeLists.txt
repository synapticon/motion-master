# The HTTP client Motion Master uses for its own outbound calls: the certificate download and the
# auto-tuning child process. libcurl is linked privately, so no curl type reaches a header and no
# other target inherits the dependency. That is also why this is its own library rather than a file
# in mm::core, which every target links.
add_library(net STATIC http_client.cc)
add_library(mm::net ALIAS net)

target_include_directories(net PUBLIC ${CMAKE_SOURCE_DIR}/libs)

find_package(CURL REQUIRED)

target_link_libraries(net PRIVATE CURL::libcurl)

add_subdirectory(tests)
