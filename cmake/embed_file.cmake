# Generate a self-contained header that embeds INPUT verbatim as `inline const std::string_view
# mm::<VAR>`.
#
# Invoked at build time via `cmake -P` (see apps/motion_master/CMakeLists.txt): cmake -DINPUT=<file>
# -DOUTPUT=<file.h> -DVAR=<name> -P embed_file.cmake
#
# Header-only: `inline` gives one linker-deduped definition, so no separate .cc and no hand-written
# declaration header are needed. A HEX byte array (not a string literal) is used so arbitrary
# content — quotes, backslashes, non-ASCII — round-trips byte-for-byte with no escaping hazards and
# no per-compiler string-literal length limit. The byte array is `constexpr`; the view over it is
# `inline const` (not `constexpr`) because the `reinterpret_cast` from `unsigned char*` to `char*`
# is not a constant expression — its dynamic init is a single pointer+length store depending only on
# the constant-initialized array, so there is no static-init-order hazard.
#
# Header-only is right when the blob has ONE consumer (as swagger.yml does — only http_server.cc):
# the byte array is then parsed by the compiler exactly once, and deleting the declaration header is
# a clean win. It stops being right once the same blob is #included by many translation units — the
# linker still keeps one copy of the data, but every including TU re-parses the whole (here 135 KB)
# array, so compile time scales with the number of includers. At that point emit a `.cc` that
# DEFINES the blob and a small header that only `extern`-DECLARES the view (`extern const
# std::string_view mm::<VAR>;`), add the `.cc` to the target's sources, and the array is parsed in
# that one TU no matter how widely the declaration spreads. (That was this file's prior form, kept
# in git history — switch back to it if a second consumer ever appears.)

file(READ "${INPUT}" hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")

file(
  WRITE "${OUTPUT}"
  "// Generated from ${INPUT} by cmake/embed_file.cmake — do not edit.\n"
  "#pragma once\n"
  "#include <string_view>\n"
  "namespace mm {\n"
  "// unsigned so non-ASCII bytes (e.g. UTF-8 em dashes) don't narrow.\n"
  "inline constexpr unsigned char ${VAR}Bytes[] = {${bytes}};\n"
  "inline const std::string_view ${VAR}{\n"
  "    reinterpret_cast<const char*>(${VAR}Bytes), sizeof(${VAR}Bytes)};\n"
  "}  // namespace mm\n")
