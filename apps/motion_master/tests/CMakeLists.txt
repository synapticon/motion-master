find_package(GTest CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(mm_app_tests
    ring_log_sink_test.cc
    monitoring_api_test.cc
    ../monitoring_api.cc
)

# App headers are included unprefixed (e.g. "ring_log_sink.h"), matching main.cc.
target_include_directories(mm_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# mm::node brings the libs include path and nlohmann_json (used by monitoring_api).
target_link_libraries(mm_app_tests PRIVATE mm::node spdlog::spdlog GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_app_tests)
