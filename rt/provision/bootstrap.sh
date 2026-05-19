#!/usr/bin/env bash
set -euo pipefail

REPO="synapticon/motion-master"
CLONE_DIR="$HOME/github/synapticon/motion-master"

# --- OS detection ---

detect_os_family() {
    if [ -f /etc/os-release ]; then
        # shellcheck source=/dev/null
        . /etc/os-release
        echo "${ID_LIKE:-$ID}"
    else
        echo "unknown"
    fi
}

OS_FAMILY=$(detect_os_family)

# --- Install git + ansible ---

install_deps() {
    if echo "$OS_FAMILY" | grep -qiE "debian|ubuntu"; then
        echo "[bootstrap] Detected Debian/Ubuntu"
        sudo apt-get update -qq
        command -v git &>/dev/null || sudo apt-get install -y git
        if ! command -v ansible-playbook &>/dev/null; then
            command -v pipx &>/dev/null || sudo apt-get install -y pipx
            pipx install ansible
            export PATH="$PATH:$HOME/.local/bin"
        fi
    else
        echo "[bootstrap] Unsupported OS: $OS_FAMILY"
        echo "Install git and ansible manually, then re-run this script."
        exit 1
    fi
}

# --- Clone repo ---

clone_repo() {
    if [ -d "$CLONE_DIR/.git" ]; then
        echo "[bootstrap] Repo already cloned at $CLONE_DIR"
        return
    fi

    mkdir -p "$(dirname "$CLONE_DIR")"
    echo "[bootstrap] Cloning $REPO ..."
    git clone "https://github.com/${REPO}.git" "$CLONE_DIR"
}

# --- Run playbook ---

run_playbook() {
    echo ""
    echo "[bootstrap] Running Ansible playbook ..."
    echo "You will be prompted for your sudo password."
    echo ""
    "$CLONE_DIR/rt/provision/play.sh"
}

install_deps
clone_repo
run_playbook
