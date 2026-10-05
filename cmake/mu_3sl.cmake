# The iC-Haus MU_3SL library computes the iC-MU track corrections. It is vendored as headers and
# prebuilt binaries, and iC-Haus builds it for Linux x86_64 and Windows x64 only. On every other
# platform this file defines no target, and MM_HAVE_MU_3SL stays off.
#
# Linux loads the library by its soname, libMU_3SL_interface.so.3. The build copies the file next to
# each executable under that name, and the executable finds it through an $ORIGIN run path. So a
# package carries one file and no symlink.

set(MU_3SL_DIR "${CMAKE_SOURCE_DIR}/extern/MU_3SL-3.4.2")
set(MM_HAVE_MU_3SL OFF)

if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64)$")
  add_library(mu_3sl SHARED IMPORTED GLOBAL)
  set_target_properties(
    mu_3sl
    PROPERTIES IMPORTED_LOCATION "${MU_3SL_DIR}/bin/linux/x86_64/libMU_3SL_interface.so.3.4.2"
               IMPORTED_SONAME libMU_3SL_interface.so.3)
  set(MU_3SL_RUNTIME_NAME libMU_3SL_interface.so.3)
  set(MM_HAVE_MU_3SL ON)
elseif(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 8)
  add_library(mu_3sl SHARED IMPORTED GLOBAL)
  set_target_properties(
    mu_3sl PROPERTIES IMPORTED_LOCATION "${MU_3SL_DIR}/bin/windows/x86-64/MU_3SL_interface_64.dll"
                      IMPORTED_IMPLIB "${MU_3SL_DIR}/bin/windows/x86-64/MU_3SL_interface_64.lib")
  set(MU_3SL_RUNTIME_NAME MU_3SL_interface_64.dll)
  set(MM_HAVE_MU_3SL ON)
else()
  message(STATUS "MU_3SL: no build for ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}, "
                 "so the iC-MU calibration is not available")
  return()
endif()

add_library(mm::mu_3sl ALIAS mu_3sl)
# SYSTEM, because the vendor headers do not build under -Wall -Wextra -Wpedantic -Werror.
target_include_directories(mu_3sl SYSTEM INTERFACE "${MU_3SL_DIR}/include")
target_compile_definitions(mu_3sl INTERFACE MM_HAVE_MU_3SL)

# Links MU_3SL into an executable and copies the runtime file next to it, so the executable starts
# from the build tree and from a package with no other copy of the library.
function(mm_link_mu_3sl target)
  target_link_libraries(${target} PRIVATE mm::mu_3sl)
  add_custom_command(
    TARGET ${target}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:mm::mu_3sl>
            "$<TARGET_FILE_DIR:${target}>/${MU_3SL_RUNTIME_NAME}"
    COMMENT "Copying ${MU_3SL_RUNTIME_NAME} next to ${target}"
    VERBATIM)
  # The packages ship the executable from the build tree, so the build tree run path is the one that
  # ships. BUILD_WITH_INSTALL_RPATH keeps the absolute path of extern/ out of it.
  if(NOT WIN32)
    set_target_properties(${target} PROPERTIES BUILD_WITH_INSTALL_RPATH ON INSTALL_RPATH "$ORIGIN")
  endif()
endfunction()
