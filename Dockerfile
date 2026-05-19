# syntax=docker/dockerfile:1
#
# Build prerequisite: initialise the vcpkg submodule before building this image.
#   git submodule update --init --recursive
#
# Build:
#   docker build -t motion-master .
#
# Run (self-signed cert auto-generated):
#   docker run --rm --network host motion-master
#
# The server binds to 127.0.0.1 (loopback) by design, so --network host is
# required — Docker's port-forwarding routes through eth0, not loopback, and
# would never reach the server.  Pass --port <n> to avoid collisions:
#   docker run --rm --network host motion-master --port 9443
#
# Run with real certs:
#   docker run --rm --network host \
#     -e CERT=/certs/cert.pem -e KEY=/certs/key.pem \
#     -v /path/to/cert.pem:/certs/cert.pem:ro \
#     -v /path/to/key.pem:/certs/key.pem:ro \
#     motion-master
#
# RT scheduling: the GameLoop calls pthread_setschedparam(SCHED_FIFO, 80) and
# mlockall().  Both degrade gracefully — if either call fails the server still
# starts, but logs a warning and runs without RT guarantees.
#
# To enable real-time scheduling the HOST kernel must be PREEMPT_RT and the
# container needs two capabilities:
#   docker run --cap-add SYS_NICE --cap-add IPC_LOCK --ulimit memlock=-1 ...
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

WORKDIR /app
COPY --from=build /src/build/x64-linux-release/apps/motion_master/motion-master .
COPY --from=build /src/apps/motion_master/swagger.yml .

EXPOSE 8443

COPY docker-entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
