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
# would never reach the server.  Pass --port <n> to avoid collisions:
#   docker run --rm --network host motion-master --port 9443
#
# TLS certificate discovery order (same as tools/run.sh):
#   1. CERT / KEY env vars — explicit override, highest priority
#   2. cert.pem / key.pem baked into the image — present on release images
#      (CI places them at the repo root before docker build)
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
#   EtherCAT only:
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW --network host motion-master \
#                --driver soem --adapter eth0
#
#   RT only (host kernel must be PREEMPT_RT):
#     docker run --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
#                --network host motion-master
#
#   Full EtherCAT + RT:
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW \
#                --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
#                --network host motion-master --driver soem --adapter eth0
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

# cert.pem / key.pem are not committed to git. In release builds they are
# placed at the repo root by CI before docker build and end up here. In
# developer builds they are absent; create empty placeholders so the COPY
# in the runtime stage always has a source file.
RUN [ -f cert.pem ] || touch cert.pem; [ -f key.pem ] || touch key.pem

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

# 61447 = HTTP API, 62281 = realtime WebSocket (separate loop/port so a slow HTTP request can't
# stall the monitoring/control stream).
EXPOSE 61447 62281

COPY docker-entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
