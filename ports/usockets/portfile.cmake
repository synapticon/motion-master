# Overlay port: pins usockets 0.8.8 (same source as the vcpkg registry port) and
# disables TLS 1.3 session tickets on every server SSL_CTX. See the
# vcpkg_replace_string below for the full rationale; in short, the post-handshake
# NewSessionTicket write makes OpenSSL return spurious WANT_READ on a non-blocking
# socket, deadlocking uSockets' epoll loop and causing intermittent (~25%)
# WebSocket-upgrade hangs on port 62281 with TLS 1.3 clients (TLS 1.2 is
# unaffected because it has no post-handshake ticket exchange). The bug is unfixed
# upstream (v0.8.8 is the latest tag), so we carry the change here.

# Upstream only support static compilation,
# https://github.com/uNetworking/uSockets/commit/b950efd6b10f06dd3ecb5b692e5d415f48474647
vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO uNetworking/uSockets
    REF "v${VERSION}"
    SHA512 726b1665209d0006d6621352c12019bbab22bed75450c5ef1509b409d3c19c059caf94775439d3b910676fa2a4a790d490c3e25e5b8141423d88823642be7ac7
    HEAD_REF master
)
file(COPY "${CURRENT_PORT_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")
file(COPY "${CURRENT_PORT_DIR}/unofficial-usockets-config.cmake" DESTINATION "${SOURCE_PATH}")

# --- TLS 1.3 session-ticket fix (create_ssl_context_from_options) -------------
#
# In TLS 1.3 the handshake ends with the client, after which the server sends
# post-handshake NewSessionTicket message(s). That post-handshake *write* makes
# OpenSSL's SSL_read/SSL_write return SSL_ERROR_WANT_READ for internal-protocol
# reasons (not because app data is pending). uSockets' non-blocking epoll loop
# then waits for a socket event that never arrives -- nothing more is on the wire,
# because the client is waiting for the WebSocket-upgrade 101 -- and the
# connection hangs until the client times out. It is intermittent (~25%) and
# TLS-1.3-only (TLS 1.2 has no post-handshake ticket exchange). See OpenSSL
# issues #7327 and #6234.
#
# Session tickets only enable TLS session *resumption* (a perf optimization);
# disabling them removes the post-handshake server write entirely, so the
# deadlock cannot occur. For a localhost/LAN control server, resumption buys
# essentially nothing (every connection already completes a full handshake in a
# few ms). SSL_CTX_set_num_tickets is OpenSSL 1.1.1+.
vcpkg_replace_string(
    "${SOURCE_PATH}/src/crypto/openssl.c"
    "    SSL_CTX *ssl_context = SSL_CTX_new(TLS_method());"
    "    SSL_CTX *ssl_context = SSL_CTX_new(TLS_method());

    // motion-master: disable TLS 1.3 NewSessionTicket. The post-handshake ticket
    // write makes OpenSSL return spurious WANT_READ on our non-blocking socket,
    // deadlocking the epoll loop and hanging ~25% of TLS 1.3 WebSocket upgrades
    // (OpenSSL #7327/#6234). Tickets only enable session resumption, which a
    // localhost/LAN control server does not need.
    SSL_CTX_set_num_tickets(ssl_context, 0);")
# ------------------------------------------------------------------------------

vcpkg_check_features(OUT_FEATURE_OPTIONS FEATURE_OPTIONS
    FEATURES
        ssl     WITH_OPENSSL
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        ${FEATURE_OPTIONS}
)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-usockets)
vcpkg_copy_pdbs()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
