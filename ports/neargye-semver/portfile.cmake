# The registry port of neargye-semver with one patch.
#
# The library's leading-zero check runs on every digit of a prerelease identifier, not only on
# its first, so it rejects a numeric identifier with a zero in its second or a later position:
# 6.0.0-alpha.100 is refused while 6.0.0-alpha.99 and 6.0.0-alpha.110 pass. SemVer 2.0 forbids a
# leading zero only. The patch applies the check to the first character of an identifier. Found
# when the version 6.0.0-alpha.100 failed the static assertion in libs/core/version.h.in.
set(VCPKG_BUILD_TYPE release) # header-only port

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO Neargye/semver
    REF v${VERSION}
    SHA512 194f679224a371a4434bc32f14717ef0f83c796e878a3ada4aa2e8c925e5e64aaa63f703d891a8ae6b15452e16cf714983e3b0a15e37185275e82e5120393f44
    HEAD_REF master
    PATCHES leading-zero-only-at-identifier-start.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
      -DSEMVER_OPT_INSTALL=ON
      -DSEMVER_OPT_BUILD_EXAMPLES=OFF
      -DSEMVER_OPT_BUILD_TESTS=OFF
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME semver CONFIG_PATH "lib/cmake/semver")
vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/lib") # empty; rm for vcpkg validity checks
