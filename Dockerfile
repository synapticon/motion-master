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
# TLS certificate discovery (same priority as tools/run.sh):
#   1. -e CERT=<path> -e KEY=<path>  explicit override
#   2. cert.pem / key.pem baked into the image (release builds only — CI places
#      them at the repo root before docker build)
#   3. Mount the acme.sh directory from the host (developer builds):
#        docker run --rm --network host \
#          -v "$HOME/.acme.sh/local.motion-master.synapticon.com_ecc:/root/.acme.sh/local.motion-master.synapticon.com_ecc:ro" \
#          motion-master
#   4. Self-signed fallback — generated at startup (browser security exception required)
#
# Capabilities — grant only what is needed:
#
#   EtherCAT raw sockets (NET_ADMIN + NET_RAW):
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW ...
#
#   RT scheduling / mlockall (SYS_NICE + IPC_LOCK); HOST kernel must be PREEMPT_RT:
#     docker run --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 ...
#
#   Full EtherCAT + RT:
#     docker run --cap-add NET_ADMIN --cap-add NET_RAW \
#                --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 \
#                --network host motion-master
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
COPY --from=build /src/apps/motion_master/swagger.yml .

EXPOSE 8443

COPY docker-entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
