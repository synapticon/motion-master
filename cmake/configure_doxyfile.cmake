# Called from CI as: cmake -D MM_VERSION=x -D MM_SOURCE_DIR=<root> -D MM_BINARY_DIR=<build> -P
# cmake/configure_doxyfile.cmake CMAKE_SOURCE_DIR is overridden here because script mode sets it to
# the script's own directory.
set(CMAKE_SOURCE_DIR "${MM_SOURCE_DIR}")
set(CMAKE_CURRENT_BINARY_DIR "${MM_BINARY_DIR}")
configure_file("${MM_SOURCE_DIR}/Doxyfile.in" "${MM_BINARY_DIR}/Doxyfile" @ONLY)
