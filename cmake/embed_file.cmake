# Generate a .cc that embeds INPUT verbatim as `extern const std::string_view mm::<VAR>`.
#
# Invoked at build time via `cmake -P` (see apps/motion_master/CMakeLists.txt):
#   cmake -DINPUT=<file> -DOUTPUT=<file.cc> -DVAR=<name> -DHEADER=<header.h> -P embed_file.cmake
#
# Uses a HEX byte array (not a raw string literal) so arbitrary content — quotes,
# backslashes, non-ASCII — round-trips byte-for-byte with no escaping hazards.

file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")

file(
  WRITE "${OUTPUT}"
  "// Generated from ${INPUT} by cmake/embed_file.cmake — do not edit.\n"
  "#include \"${HEADER}\"\n"
  "namespace {\n"
  "// unsigned so non-ASCII bytes (e.g. UTF-8 em dashes in the spec) don't narrow.\n"
  "constexpr unsigned char kData[] = {${bytes}};\n"
  "}  // namespace\n"
  "namespace mm {\n"
  "const std::string_view ${VAR}{reinterpret_cast<const char*>(kData), sizeof(kData)};\n"
  "}  // namespace mm\n")
