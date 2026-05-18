cmake_minimum_required(VERSION 4.0)

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MM_VERSION)

project(motion-master LANGUAGES CXX)

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
add_subdirectory(hil)

include(cmake/lint.cmake)
include(cmake/docs.cmake)
