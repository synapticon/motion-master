cmake_minimum_required(VERSION 4.0)
project(motion-master VERSION 6.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /WX /utf-8)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

add_subdirectory(libs/core)
add_subdirectory(libs/comm)
add_subdirectory(apps/motion_master)
add_subdirectory(apps/playground)

include(cmake/lint.cmake)
