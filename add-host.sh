#!/usr/bin/env bash
# Point this computer's browser at a Motion Master running on another machine (Linux, macOS).
#
#   sudo ./add-host.sh 192.168.1.50
#   sudo ./add-host.sh --remove 192.168.1.50
#
# Motion Master's certificate cannot cover a bare IP address — no CA issues those — so a server on
# the network is reached under a hostname that encodes its address and that the bundled certificate
# does cover:
#
#   192.168.1.50   ->   192-168-1-50.ip.motion-master.synapticon.com
#
# Those names are deliberately absent from public DNS, so this script maps one to its address in
# /etc/hosts. That is enough, and it is not a security compromise: TLS validates a certificate
# against the *name*, never against how the name was resolved, so the result is an ordinary trusted
# HTTPS connection with no warning to click through.
#
# RUN THIS ON THE MACHINE WITH THE BROWSER, not on the machine running Motion Master. Name
# resolution happens at the requesting end — the server never looks up its own name — so an entry on
# the server would achieve nothing, and every computer that opens the Console needs its own.
set -euo pipefail

readonly DOMAIN="ip.motion-master.synapticon.com"
readonly HOSTS="${HOSTS_FILE:-/etc/hosts}"  # overridable so the script can be exercised safely
readonly MARKER="# motion-master"
readonly HTTP_PORT=61447

usage() {
  sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

die() {
  echo "error: $*" >&2
  exit 1
}

# Dotted quad, each octet 0-255. Rejects hostnames and partial addresses, so a typo is an error
# rather than a nonsense hosts entry that fails confusingly later.
is_ipv4() {
  local ip=$1 octet
  [[ $ip =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || return 1
  for octet in ${ip//./ }; do
    ((octet <= 255)) || return 1
  done
  return 0
}

has_entry() {
  grep -qF " $1 $MARKER" "$HOSTS"
}

# Rewrites the file in place rather than renaming a temporary over it, so /etc/hosts keeps its
# original owner, mode and any ACLs.
remove_entry() {
  local host=$1 tmp
  tmp=$(mktemp)
  grep -vF " $host $MARKER" "$HOSTS" >"$tmp"
  cat "$tmp" >"$HOSTS"
  rm -f "$tmp"
}

require_writable() {
  [[ -w $HOSTS ]] || die "$HOSTS is not writable — re-run with sudo"
}

main() {
  local remove=0 ip=""
  while (($#)); do
    case $1 in
      -h | --help) usage 0 ;;
      --remove) remove=1 ;;
      -*) die "unknown option: $1 (try --help)" ;;
      *) ip=$1 ;;
    esac
    shift
  done

  [[ -n $ip ]] || usage 1
  is_ipv4 "$ip" || die "'$ip' is not an IPv4 address"

  local host="${ip//./-}.$DOMAIN"
  local line="$ip $host $MARKER"

  if ((remove)); then
    if has_entry "$host"; then
      require_writable
      remove_entry "$host"
      echo "Removed $host from $HOSTS"
    else
      echo "No entry for $host in $HOSTS — nothing to remove."
    fi
    return
  fi

  if grep -qxF "$line" "$HOSTS"; then
    echo "$HOSTS already maps $host to $ip"
  else
    require_writable
    # A stale mapping for this same name must go first: the resolver takes the first match in the
    # file, so an old line would silently win over the one appended below.
    if has_entry "$host"; then
      remove_entry "$host"
    fi
    cp "$HOSTS" "$HOSTS.motion-master.bak"
    printf '%s\n' "$line" >>"$HOSTS"
    echo "Added to $HOSTS (previous copy saved as $HOSTS.motion-master.bak):"
    echo "  $line"
  fi

  echo
  echo "Open https://$host:$HTTP_PORT — or set the Console's Host field to:"
  echo "  $host"
  echo
  echo "The server must be running with server.bindAddress set to \"0.0.0.0\"."
}

main "$@"
