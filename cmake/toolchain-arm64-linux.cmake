set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(CMAKE_C_COMPILER aarch64-linux-gnu-gcc)
set(CMAKE_CXX_COMPILER aarch64-linux-gnu-g++)

# Fedora ships the ARM64 glibc sysroot at a non-default location that doesn't
# match the compiler's --with-sysroot. Set it explicitly so the linker finds
# crt1.o/libc. On Debian/Ubuntu the compiler's baked-in sysroot is correct
# and no override is needed.
if(EXISTS "/usr/aarch64-redhat-linux/sys-root/fc44")
    set(CMAKE_SYSROOT "/usr/aarch64-redhat-linux/sys-root/fc44")
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
