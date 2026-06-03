# Windows packaging — CPack NSIS installer.
#
# Included from the root CMakeLists only on WIN32 so the Linux deb/rpm flow
# (tools/package.sh) is untouched.  Invoke after building with:
#   cpack -G NSIS --config build/<preset>/CPackConfig.cmake
# or simply `cpack -G NSIS` from the build directory.  Produces
#   motion-master-<version>-windows-x64-setup.exe
#
# Runtime dependency: SOEM's raw-EtherCAT NIC driver needs Npcap (npcap.com).
# We do not bundle it (redistribution licensing); instead the installer detects
# it and, if missing, offers to open the download page.

set(CPACK_GENERATOR "NSIS")
set(CPACK_PACKAGE_NAME "Motion Master")
set(CPACK_PACKAGE_VENDOR "Synapticon")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Motion control software for SOMANET servo drives")
set(CPACK_PACKAGE_VERSION "${MM_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "Motion Master")
set(CPACK_PACKAGE_FILE_NAME "motion-master-${MM_VERSION}-windows-x64-setup")

# NSIS needs numeric version components; derive them from VERSION (drops any
# -alpha.N suffix).
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)" _mm_ver "${MM_VERSION}")
set(CPACK_PACKAGE_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(CPACK_PACKAGE_VERSION_MINOR "${CMAKE_MATCH_2}")
set(CPACK_PACKAGE_VERSION_PATCH "${CMAKE_MATCH_3}")

# Start-menu shortcut: "motion-master.exe" -> "Motion Master".
set(CPACK_PACKAGE_EXECUTABLES "motion-master" "Motion Master")

set(CPACK_NSIS_PACKAGE_NAME "Motion Master ${MM_VERSION}")
set(CPACK_NSIS_DISPLAY_NAME "Motion Master ${MM_VERSION}")
set(CPACK_NSIS_URL_INFO_ABOUT "https://motion-master.synapticon.com")
set(CPACK_NSIS_ENABLE_UNINSTALL_BEFORE_INSTALL ON)
set(CPACK_NSIS_MODIFY_PATH OFF)

# Detect Npcap; if absent, offer to open the download page.  Injected verbatim
# into the installer's install section (bracket string keeps backslashes literal).
set(CPACK_NSIS_EXTRA_INSTALL_COMMANDS [==[
  ReadRegStr $0 HKLM "SOFTWARE\WOW6432Node\Npcap" ""
  StrCmp $0 "" 0 npcap_present
  IfFileExists "$SYSDIR\Npcap\wpcap.dll" npcap_present
  IfFileExists "$SYSDIR\wpcap.dll" npcap_present
    MessageBox MB_YESNO "Motion Master needs Npcap to access EtherCAT network adapters, but it does not appear to be installed.$\r$\n$\r$\nOpen the Npcap download page now?" IDNO npcap_present
    ExecShell "open" "https://npcap.com/#download"
  npcap_present:
]==])

include(CPack)
