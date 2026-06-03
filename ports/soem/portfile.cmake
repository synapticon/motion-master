vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO OpenEtherCATsociety/SOEM
    REF 304d1c05eab77dc0d426f1a5cf09c8cc7dc03713
    SHA512 b96f4ccc9d205b06a00a4739a05c16a995e4cbf01310b475cd708e8bd7965ebc5d2d54a756567e9708346b6254c181d8e5b26e0c9bfdebdfaa2dd8a8acff6b82
    HEAD_REF master
)

# macOS (Darwin) support.  Upstream SOEM 2.0 ships only Linux/Windows platform
# layers; the build does `include(cmake/${CMAKE_SYSTEM_NAME}.cmake)`, so a stock
# configure on macOS fails for lack of cmake/Darwin.cmake.  We provide one here
# plus a 2.0-compatible osal/macosx.  The oshw/macosx (BPF/libpcap NIC driver)
# already lives in upstream contrib/ and is already on the 2.0 API, so we just
# move it into place.  These copies are inert on Linux/Windows — cmake/Darwin.cmake
# is only include()d when CMAKE_SYSTEM_NAME is Darwin.
file(COPY "${SOURCE_PATH}/contrib/oshw/macosx" DESTINATION "${SOURCE_PATH}/oshw")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/files/osal_macosx/" DESTINATION "${SOURCE_PATH}/osal/macosx")
file(COPY "${CMAKE_CURRENT_LIST_DIR}/files/Darwin.cmake" DESTINATION "${SOURCE_PATH}/cmake")

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DSOEM_BUILD_SAMPLES=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()

vcpkg_cmake_config_fixup(CONFIG_PATH "cmake")

file(REMOVE_RECURSE
    "${CURRENT_PACKAGES_DIR}/bin"
    "${CURRENT_PACKAGES_DIR}/debug/bin"
    "${CURRENT_PACKAGES_DIR}/debug/include"
    "${CURRENT_PACKAGES_DIR}/debug/share"
    "${CURRENT_PACKAGES_DIR}/scripts"
    "${CURRENT_PACKAGES_DIR}/debug/scripts"
)

file(REMOVE
    "${CURRENT_PACKAGES_DIR}/README.md"
    "${CURRENT_PACKAGES_DIR}/debug/README.md"
    "${CURRENT_PACKAGES_DIR}/debug/LICENSE.md"
)

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE.md")
