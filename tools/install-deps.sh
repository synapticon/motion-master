#!/usr/bin/env bash
#
# Install everything needed to build, test, lint, package and provision Motion
# Master. Supports Debian/Ubuntu (apt) and Fedora (dnf).
#
# Usage:
#   ./tools/install-deps.sh              # install everything
#   ./tools/install-deps.sh --dry-run    # print what would be installed
#   ./tools/install-deps.sh --no-qemu    # skip virtualization (rt/ won't work)
#   ./tools/install-deps.sh --no-docker  # skip Docker (hil/api tests won't work)
#
# Not covered here, on purpose:
#   * vcpkg dependencies — built from source by the build itself; this script
#     installs the tools that build needs (autoconf, libtool, perl, ...).
#   * pnpm — comes from Node's bundled corepack, not the distro. See the end.
#   * The vcpkg submodule: git submodule update --init --recursive

set -euo pipefail

DRY_RUN=false
WITH_QEMU=true
WITH_DOCKER=true

while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run)   DRY_RUN=true ;;
        --no-qemu)   WITH_QEMU=false ;;
        --no-docker) WITH_DOCKER=false ;;
        # Print the header comment, stopping at the first line of code, so this
        # cannot drift out of sync with a fixed line range.
        -h|--help)   awk 'NR>1 && !/^#/{exit} NR>1{sub(/^# ?/, ""); print}' "$0"; exit 0 ;;
        *) echo "unknown option: $1 (try --help)" >&2; exit 1 ;;
    esac
    shift
done

log() { printf '\n\033[1m[deps] %s\033[0m\n' "$*"; }
note() { printf '[deps] %s\n' "$*"; }
die() { printf '[deps] error: %s\n' "$*" >&2; exit 1; }

# Docker and Node are routinely installed from outside the distro — docker-ce
# from Docker's own repository, Node via nvm/fnm or a versioned Fedora package.
# Those installations *conflict* with the distro packages below (moby-engine
# against docker-ce, nodejs22 against nodejs20-bin), so a blind install would
# either fail the transaction or swap out something that already works. If the
# command is already there, leave it alone.
have() { command -v "$1" >/dev/null 2>&1; }

# --- which distro ------------------------------------------------------------

[ -r /etc/os-release ] || die "cannot read /etc/os-release"
# shellcheck source=/dev/null
. /etc/os-release

case " ${ID:-} ${ID_LIKE:-} " in
    *" debian "*|*" ubuntu "*) FAMILY=debian ;;
    *" fedora "*|*" rhel "*)   FAMILY=fedora ;;
    *) die "unsupported distribution '${ID:-unknown}' — this script covers Debian/Ubuntu and Fedora" ;;
esac

log "detected ${PRETTY_NAME:-$ID} (${FAMILY})"

# --- package sets ------------------------------------------------------------
#
# Grouped by what breaks without them, so a group can be dropped knowingly.
# Names differ enough between the two families to be worth spelling out in full
# rather than mapping — a table that lies is worse than two lists.

if [ "$FAMILY" = debian ]; then
    # C++23 toolchain. Debian 13 (trixie) ships GCC 14 as the default behind
    # build-essential; std::expected needs 13 or newer.
    packages=(build-essential gcc g++ cmake ninja-build pkg-config)

    # What vcpkg needs in order to build the dependencies from source.
    packages+=(git curl zip unzip tar xz-utils autoconf automake libtool perl)

    # tools/check.sh — format, cppcheck, lint. cpplint and cmake-format are
    # Python and come further down.
    packages+=(clang clang-format clang-tidy cppcheck shellcheck)

    # The ThreadSanitizer runtime, for the x64-linux-tsan preset (see docs/LOCKING.md).
    # GCC needs libtsan installed separately; clang's ships with the compiler.
    packages+=(libtsan2)

    # tools/package.sh builds both a .deb and an .rpm, on either distro.
    packages+=(dpkg-dev rpm)

    # web/ workspace (Console, client library) and hil/api tests. Checked
    # separately: adding both when only one is missing would pull the distro's
    # nodejs alongside an existing nvm/fnm or versioned install.
    have node || packages+=(nodejs)
    have npm || packages+=(npm)

    # hil/jitter_bench's plot script, and vcpkg's bootstrap.
    packages+=(python3 python3-pip python3-venv python3-matplotlib)

    # rt/ provisioning: Ansible over SSH, plus socat for QEMU's monitor socket.
    packages+=(ansible openssh-client socat)

    # Used by CI scripts and the actions-runner role.
    packages+=(jq gh ca-certificates)

    if [ "$WITH_QEMU" = true ]; then
        # qemu-system-arm carries the aarch64 emulator on Debian, despite the
        # name. qemu-user-static + binfmt let an arm64 binary run directly on an
        # x86 host, which is how a Pi image is prepared without a Pi.
        packages+=(qemu-system-x86 qemu-system-arm qemu-utils
                   qemu-user-static binfmt-support qemu-efi-aarch64
                   genisoimage xorriso)
    fi

    if [ "$WITH_DOCKER" = true ] && ! have docker; then
        packages+=(docker.io docker-buildx)
    fi

    install_cmd=(sudo apt-get install -y "${packages[@]}")
    refresh_cmd=(sudo apt-get update)
else
    packages=(gcc gcc-c++ make cmake ninja-build pkgconf-pkg-config)
    packages+=(git curl zip unzip tar xz autoconf automake libtool perl)
    packages+=(clang clang-tools-extra cppcheck ShellCheck)
    # ThreadSanitizer runtime for GCC (x64-linux-tsan preset; clang bundles its own).
    packages+=(libtsan)
    packages+=(dpkg-dev rpm-build)
    have node || packages+=(nodejs)
    have npm || packages+=(npm)
    packages+=(python3 python3-pip python3-matplotlib)
    packages+=(ansible openssh-clients socat)
    packages+=(jq gh ca-certificates)

    # Lets a locally built binary run on an older distro (Debian 13, the boards)
    # without dragging in this machine's newer libstdc++:
    #   cmake ... -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
    packages+=(libstdc++-static)

    if [ "$WITH_QEMU" = true ]; then
        packages+=(qemu-system-x86 qemu-system-aarch64 qemu-img
                   qemu-user-static edk2-aarch64
                   genisoimage xorriso)
    fi

    # Fedora defaults to podman; Docker proper is moby-engine. hil/api drives
    # containers through the docker CLI, so install the engine rather than
    # relying on a podman-docker shim. Skipped when docker is already present —
    # commonly docker-ce from Docker's repository, which moby-engine conflicts
    # with (as does containerd.io against moby-engine's containerd).
    if [ "$WITH_DOCKER" = true ] && ! have docker; then
        packages+=(moby-engine docker-cli)
    fi

    install_cmd=(sudo dnf install -y "${packages[@]}")
    refresh_cmd=(true)
fi

# Needed by tools/lint.sh and tools/format-cmake.sh, and not packaged by either
# distro under a name worth relying on. pipx keeps them out of the system
# Python, which PEP 668 makes awkward to write to anyway.
pipx_packages=(cpplint cmakelang)

# Say what was left out, so a skip never looks like an oversight.
have docker && [ "$WITH_DOCKER" = true ] &&
    note "docker already installed ($(docker --version 2>/dev/null | head -1)) — leaving it alone"
have node &&
    note "node already installed ($(node --version 2>/dev/null)) — leaving it alone"

# --- go ----------------------------------------------------------------------

if [ "$DRY_RUN" = true ]; then
    log "would install ${#packages[@]} packages:"
    printf '  %s\n' "${packages[@]}"
    log "would install via pipx:"
    printf '  %s\n' "${pipx_packages[@]}"
    exit 0
fi

log "refreshing package metadata"
"${refresh_cmd[@]}"

log "installing ${#packages[@]} packages"
"${install_cmd[@]}"

log "installing Python tooling with pipx"
if ! command -v pipx >/dev/null 2>&1; then
    if [ "$FAMILY" = debian ]; then
        sudo apt-get install -y pipx
    else
        sudo dnf install -y pipx
    fi
fi
for p in "${pipx_packages[@]}"; do
    pipx install "$p" 2>/dev/null || pipx upgrade "$p" >/dev/null 2>&1 || true
done
pipx ensurepath >/dev/null 2>&1 || true

# --- report ------------------------------------------------------------------

log "checking what landed"
check() {
    if command -v "$1" >/dev/null 2>&1; then
        printf '  ok    %-22s %s\n' "$1" "$(command -v "$1")"
    else
        printf '  MISS  %-22s\n' "$1"
    fi
}
for t in gcc g++ cmake ninja clang-format clang-tidy cppcheck shellcheck \
         cpplint cmake-format node npm python3 ansible-playbook jq; do
    check "$t"
done
if [ "$WITH_QEMU" = true ]; then
    for t in qemu-system-x86_64 qemu-system-aarch64 qemu-img genisoimage; do
        check "$t"
    done
fi
[ "$WITH_DOCKER" = true ] && check docker

log "next steps"
cat <<'EOF'
  git submodule update --init --recursive   # vcpkg, first time only
  corepack enable                           # provides pnpm (bundled with Node)
  cmake --preset x64-linux-debug
  cmake --build --preset x64-linux-debug

  If a "cpplint: command not found" survives this, open a new shell — pipx
  installs into ~/.local/bin, which an existing shell may not have on PATH.
EOF

if [ "$WITH_DOCKER" = true ] && ! groups | grep -qw docker; then
    printf '\n  Docker needs your user in the docker group, then a re-login:\n'
    printf '    sudo usermod -aG docker %s\n' "${USER:-$(id -un)}"
fi
