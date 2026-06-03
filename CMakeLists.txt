cmake_minimum_required(VERSION 4.0)

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MM_VERSION)

project(motion-master LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
  add_compile_options(/W4 /WX /utf-8)
  # Standard C functions (strerror, etc.) trip MSVC's C4996 "unsafe" deprecation,
  # which /WX makes fatal. The POSIX/C names are portable and intended here.
  # NOMINMAX stops <windows.h> from defining min()/max() macros that clobber
  # std::numeric_limits<>::max() in semver/uWebSockets; WIN32_LEAN_AND_MEAN trims
  # the <windows.h> surface (and keeps winsock v1 out).
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

add_subdirectory(libs/core)
add_subdirectory(libs/comm)
add_subdirectory(libs/node)
add_subdirectory(apps/motion_master)
add_subdirectory(apps/playground)
add_subdirectory(hil)

include(cmake/lint.cmake)
include(cmake/docs.cmake)

# Windows installer (CPack/NSIS).  Linux packaging stays in tools/package.sh.
if(WIN32)
  include(cmake/packaging-windows.cmake)
endif()
