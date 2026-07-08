# syntax=docker/dockerfile:1
#
# Build prerequisite: initialise the vcpkg submodule before building this image.
#   git submodule update --init --recursive
#
# Build:
#   docker build -t motion-master .
#
# The server binds to 127.0.0.1 (loopback) by design, so --network host is
# required — Docker's port-forwarding routes through eth0, not loopback, and
# would never reach the server.
#
# Settings (ports, fieldbus, log level, TLS) have no CLI flags — they are set only via a JSONC
# config file. Mount one and point MM_CONFIG at it:
#   docker run --rm --network host \
#     -v /path/to/motion-master.jsonc:/config.jsonc:ro -e MM_CONFIG=/config.jsonc motion-master
#
# TLS certificate discovery order (same as tools/run.sh):
#   1. CERT / KEY env vars — explicit override, highest priority
#   2. cert.pem / key.pem baked into the image — fetched from the rolling tls-cert
#      release at build time (empty only if that build ran offline)
#   3. ~/.acme.sh/local.motion-master.synapticon.com_ecc/ — mount from host
#      for developer builds:
#        docker run --rm --network host \
#          -v "$HOME/.acme.sh/local.motion-master.synapticon.com_ecc:/root/.acme.sh/local.motion-master.synapticon.com_ecc:ro" \
#          motion-master
#   4. Self-signed fallback — generated at startup (browser security exception required)
#
# Updating expired certs on a release image:
#   The bundled cert is renewed monthly but an older image will keep its original cert.
#   Override at runtime by mounting new certs over the bundled path —
#   the volume shadows the baked-in files:
#     docker run --rm --network host \
#       -v /path/to/cert.pem:/opt/motion-master/cert.pem:ro \
#       -v /path/to/key.pem:/opt/motion-master/key.pem:ro \
#       motion-master
#   Or use env vars to point to an arbitrary mount path:
#     docker run --rm --network host \
#       -e CERT=/certs/cert.pem -e KEY=/certs/key.pem \
#       -v /path/to/cert.pem:/certs/cert.pem:ro \
#       -v /path/to/key.pem:/certs/key.pem:ro \
#       motion-master
#
# Capabilities:
#   Docker drops most Linux capabilities by default. On bare-metal the binary is
#   stamped with setcap so file capabilities grant the required caps automatically.
#   Inside a container file capabilities are ignored — use --cap-add instead:
#
#     CAP_NET_RAW    raw/packet sockets — SOEM EtherCAT frame I/O
#     CAP_NET_ADMIN  NIC configuration — SOEM promiscuous mode
#     CAP_SYS_NICE   SCHED_FIFO scheduling — RT game loop
#     CAP_IPC_LOCK   mlockall() — pin memory for RT (also needs --ulimit memlock=-1)
#
#   The fieldbus driver/adapter are set in the mounted config's "fieldbus" block, e.g.
#   { "fieldbus": { "driver": "soem", "adapter": "eth0" } }.
#
#   EtherCAT only (config sets fieldbus):
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW --network host \
#                -v /path/to/motion-master.jsonc:/config.jsonc:ro -e MM_CONFIG=/config.jsonc \
#                motion-master
#
#   RT only (host kernel must be PREEMPT_RT):
#     docker run --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
#                --network host motion-master
#
#   Full EtherCAT + RT:
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW \
#                --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 --network host \
#                -v /path/to/motion-master.jsonc:/config.jsonc:ro -e MM_CONFIG=/config.jsonc \
#                motion-master
#
# Running with --privileged also works but grants far more than necessary.

# ── Stage 1: build ─────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -q && apt-get install -y --no-install-recommends \
        ninja-build gcc-14 g++-14 \
        curl ca-certificates git \
        zip unzip tar pkg-config \
        make perl \
    && rm -rf /var/lib/apt/lists/*

# Ubuntu 24.04 ships CMake 3.x; the project requires 4.0+.
ARG CMAKE_VERSION=4.0.0
RUN curl -fsSL \
        "https://github.com/Kitware/CMake/releases/download/v${CMAKE_VERSION}/cmake-${CMAKE_VERSION}-linux-x86_64.sh" \
        -o /tmp/cmake-install.sh \
    && bash /tmp/cmake-install.sh --skip-license --prefix=/usr/local \
    && rm /tmp/cmake-install.sh

ENV CC=gcc-14 CXX=g++-14

WORKDIR /src
COPY . .

# cert.pem / key.pem are excluded from the build context (.dockerignore). Bake the current keypair
# by fetching it from the rolling `tls-cert` release — the single source of truth (the same URL the
# running binary's self-heal uses). The keypair only authenticates local.motion-master.synapticon.com
# (→ 127.0.0.1), so baking it in is safe. If the fetch fails (offline build), bake empty placeholders
# instead: the entrypoint self-signs and the binary's startup self-heal fetches a real cert on first run.
RUN base=https://github.com/synapticon/motion-master/releases/download/tls-cert; \
    curl -fsSL "$base/cert.pem" -o cert.pem.new && curl -fsSL "$base/key.pem" -o key.pem.new \
      && mv cert.pem.new cert.pem && mv key.pem.new key.pem \
      || echo "cert fetch failed — baking empty placeholder (runtime self-heal fetches on start)"; \
    rm -f cert.pem.new key.pem.new; \
    [ -f cert.pem ] || : > cert.pem; [ -f key.pem ] || : > key.pem

# vcpkg binary cache is reused across image rebuilds when building with BuildKit.
RUN --mount=type=cache,target=/root/.cache/vcpkg/archives \
    cmake --preset x64-linux-release \
    && cmake --build --preset x64-linux-release

# Collect any vcpkg shared libraries needed at runtime (empty when all deps
# are header-only or statically linked).
RUN mkdir -p /runtime-libs \
    && find vcpkg_installed/x64-linux/lib/ -name "*.so*" \( -type f -o -type l \) \
         -exec cp -P {} /runtime-libs/ \; 2>/dev/null || true

# ── Stage 2: runtime ───────────────────────────────────────────────────────
FROM ubuntu:24.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive

# openssl: entrypoint cert generation + libssl3/libcrypto3 for uwebsockets TLS.
RUN apt-get update -q && apt-get install -y --no-install-recommends \
        openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /runtime-libs/ /usr/local/lib/
RUN ldconfig

WORKDIR /opt/motion-master
COPY --from=build /src/build/x64-linux-release/apps/motion_master/motion-master .
# Bake the cert/key fetched in the build stage (empty if that build ran offline).
COPY --from=build /src/cert.pem /src/key.pem ./

# 61447 = HTTP API, 62281 = WebSocket (separate loop/port so a slow HTTP request can't
# stall the monitoring/control stream).
EXPOSE 61447 62281

COPY docker-entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
