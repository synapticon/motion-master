#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/ansible" || exit 1

read -rsp "BECOME password: " BECOME_PASS
echo

# Provide the password via SUDO_ASKPASS so sudo authenticates without a TTY
# prompt. Ansible's --ask-become-pass flow times out on systems where PAM
# rewrites sudo's prompt — Ansible's detection regex no longer matches it.
export BECOME_PASS
ASKPASS=$(mktemp)
chmod 700 "$ASKPASS"
trap 'rm -f "$ASKPASS"' EXIT
cat > "$ASKPASS" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "$BECOME_PASS"
EOF
export SUDO_ASKPASS="$ASKPASS"

ANSIBLE_BECOME_FLAGS='-H -A' ansible-playbook site.yml "$@"
