cmake_minimum_required(VERSION 4.0)

file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" MM_VERSION)

project(motion-master LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# This codebase does not use C++20 modules and is not going to, so the dependency scan CMake runs
# for them buys nothing. Turning it off is not only a small build saving: with GCC the scan adds
# -fmodules-ts, -fdeps-format= and -fmodule-mapper= to every compile command, and those reach
# compile_commands.json, where clang-tidy reads them and refuses the translation unit outright
# ("unknown argument"). Left on, the tidy target reports a parse error per file and nothing else.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

if(MSVC)
  add_compile_options(/W4 /WX /utf-8)
  # Standard C functions (strerror, etc.) trip MSVC's C4996 "unsafe" deprecation, which /WX makes
  # fatal. The POSIX/C names are portable and intended here. NOMINMAX stops <windows.h> from
  # defining min()/max() macros that clobber std::numeric_limits<>::max() in semver/uWebSockets;
  # WIN32_LEAN_AND_MEAN trims the <windows.h> surface (and keeps winsock v1 out).
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

# Apple's libc++ still gates three C++20 facilities this codebase uses behind its experimental flag:
# std::jthread, std::stop_token (with the stop-aware std::condition_variable_any::wait overload),
# and the std::atomic<std::shared_ptr<T>> specialisation. All three are load-bearing in
# ProcedureManager — cooperative cancellation of a running procedure, and recording a run's outcome
# without taking the manager's mutex so its destructor can join a finishing thread. Without the flag
# the macOS build fails outright: "no type named 'stop_token' in namespace 'std'" and "_Atomic
# cannot be applied to type std::shared_ptr<...>".
#
# The cost is bounded, and worth stating because the flag's name invites alarm. libc++experimental
# is linked *statically*, so a shipped binary gains no runtime dependency and no ABI exposure; what
# the flag really forfeits is source stability across toolchain upgrades. A future Xcode may rename
# or retire it — that shows up as an immediate CI failure on this leg, not as a silent runtime
# problem, and the fallback is to replace the three facilities with portable equivalents (a small
# stop-token type and a mutex-guarded error slot, ~19 call sites in libs/node).
if(APPLE)
  add_compile_options(-fexperimental-library)
  add_link_options(-fexperimental-library)
endif()

enable_testing()

# SOEM's EtherCAT NIC driver on Windows talks to Npcap. We vendor the Npcap SDK (headers + import
# libs) at a fixed in-repo path so consumers link it from a stable location — SOEM's own bundled
# copy lives in the vcpkg buildtree, which does not survive binary-cache restores. comm wires it up
# (see libs/comm). Runtime still requires the Npcap driver itself (npcap.com); the installer
# prompts.
if(WIN32)
  set(NPCAP_SDK_PATH "${CMAKE_SOURCE_DIR}/extern/npcap-sdk-1.16")
  # Reusable target carrying the Npcap headers + import libs. Anything that consumes <soem/soem.h>
  # (which pulls in the win32 nicdrv.h -> <pcap.h>) links this — comm and the playground scratch app
  # both build against soem directly.
  add_library(npcap INTERFACE)
  add_library(mm::npcap ALIAS npcap)
  target_include_directories(npcap INTERFACE "${NPCAP_SDK_PATH}/Include")
  target_link_libraries(npcap INTERFACE "${NPCAP_SDK_PATH}/Lib/x64/wpcap.lib"
                                        "${NPCAP_SDK_PATH}/Lib/x64/Packet.lib")
endif()

# Ordered by dependency layer (foundation first, consumers last), not alphabetically: mm::core ->
# {mm::etg, mm::comm} -> mm::node -> mm::api -> mm::example, then the apps that consume them. CMake
# resolves link targets at generate time so any order would configure; this ordering is for
# readability, keeping the architectural layering obvious at a glance.
#
# mm::etg and mm::comm are siblings, not a chain: etg is a pure XML->struct transform over mm::core
# alone and deliberately does not depend on mm::comm (which links SOEM), so parsing an ESI file
# needs no fieldbus. mm::node is where the two meet.
add_subdirectory(libs/core)
add_subdirectory(libs/net)
add_subdirectory(libs/auto_tuning)
add_subdirectory(libs/etg)
add_subdirectory(libs/comm)
add_subdirectory(libs/node)
add_subdirectory(libs/api)
add_subdirectory(libs/example)
add_subdirectory(apps/motion_master)
add_subdirectory(apps/playground)
add_subdirectory(hil)

include(cmake/lint.cmake)
include(cmake/docs.cmake)
