find_package(GTest CONFIG REQUIRED)

add_executable(mm_core_tests
    version_test.cc
)

target_link_libraries(mm_core_tests PRIVATE mm::core GTest::gtest GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(mm_core_tests)
