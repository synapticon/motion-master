find_package(GTest CONFIG REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(spdlog CONFIG REQUIRED)

add_executable(
  mm_app_tests
  ring_log_sink_test.cc
  monitoring_api_test.cc
  config_test.cc
  cert_info_test.cc
  game_loop_test.cc
  cycle_rate_source_test.cc
  plant_model_fit_test.cc
  controller_tuning_call_test.cc
  ../monitoring_api.cc
  ../plant_model_fit.cc
  ../controller_tuning_call.cc
  ../config.cc
  ../cert_info.cc
  ../game_loop.cc
  ../cycle_rate_source.cc)

# App headers are included unprefixed (e.g. "ring_log_sink.h"), matching main.cc.
target_include_directories(mm_app_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)

# mm::node brings the libs include path and nlohmann_json (used by monitoring_api). cert_info parses
# PEM/X.509 via OpenSSL, the same library the server links for TLS.
target_link_libraries(mm_app_tests PRIVATE mm::node mm::auto_tuning spdlog::spdlog OpenSSL::Crypto
                                           GTest::gtest GTest::gtest_main)

# mm::node calls MU_3SL where it exists, so the library must sit next to the test binary before test
# discovery runs it.
if(MM_HAVE_MU_3SL)
  mm_link_mu_3sl(mm_app_tests)
endif()

# GCC 12–14 emit false-positive -Warray-bounds / -Wstringop-overflow inside libstdc++
# <bits/stl_algobase.h> when test code is inlined at -O3 (release builds). Suppress for GCC on these
# test-only targets; shipped code keeps the warnings.
target_compile_options(mm_app_tests PRIVATE $<$<CXX_COMPILER_ID:GNU>:-Wno-array-bounds
                                            -Wno-stringop-overflow>)

include(GoogleTest)
gtest_discover_tests(mm_app_tests DISCOVERY_TIMEOUT 60)
