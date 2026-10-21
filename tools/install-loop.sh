#!/usr/bin/env bash
# Runs the firmware-installation procedure on one device again and again, against a running Motion
# Master, and records every run: status, duration, the firmware version from 0x100A before and
# after, the AL state after, and the error text. Two packages are written in turn when PKG_B is
# given, so the build changes on every run. With NETEM set, netem makes the bus adapter drop or
# delay frames during each install, and the rule is off for the recovery.
#
# One run is: reset, init, scan, PRE-OP, then N installs with finalState PRE-OP, which is what MMAF
# asks for. A failed install is followed by one PRE-OP request with a 30 s timeout. The loop stops
# when the device does not come back to PRE-OP, so the drive can be power-cycled.
#
# The packages come from the firmware cache, ~/.cache/motion-master/firmwares/ on Linux, and are
# named by file name. Install the right package for the device: GET /api/devices/<position>/
# firmware-compatibility?filename=<zip> says whether it fits.
#
# Recipes, see NEXTGEN.md, Sessions 2026-10-20 and 2026-10-21:
#   NETEM="delay 25ms reorder 98% 0%"    2 % of the frames 25 ms late: late, not lost
#   NETEM="delay 150ms reorder 98% 0%"   2 % of the frames 150 ms late: lost, the FoE write fails
#   NETEM=""                             no rule, the baseline
#
# tc needs root. One line in /etc/sudoers.d/ lets this script switch the rule without a password:
#   marko ALL=(root) NOPASSWD: /usr/sbin/tc
#
# usage: NETEM="<netem parameters>" PKG_A=<zip> [PKG_B=<zip>] \
#          tools/install-loop.sh <label> [rounds] [adapter] [position]
#
# Results go to out/install-loop/<label>/: results.csv, loop.log, and the final snapshot of every
# run as round-<n>.json.
set -u
label="${1:?usage: NETEM=\"...\" PKG_A=<zip> [PKG_B=<zip>] tools/install-loop.sh <label> [rounds] [adapter] [position]}"
rounds="${2:-10}"
adapter="${3:-enp0s13f0u1u4u2}"
position="${4:-1}"
netem="${NETEM:-}"
pkg_a="${PKG_A:?set PKG_A to the file name of a package in the firmware cache}"
pkg_b="${PKG_B:-$pkg_a}"
base="${MM_BASE_URL:-https://local.motion-master.synapticon.com:61447}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$repo/out/install-loop/$label"
mkdir -p "$out"

netem_off() { sudo tc qdisc del dev "$adapter" root 2>/dev/null; }
netem_on() {
  if [[ -n "$netem" ]]; then
    # shellcheck disable=SC2086  # the parameters are several words on purpose
    sudo tc qdisc add dev "$adapter" root netem $netem
  fi
}
trap netem_off EXIT

c() { curl -sk --max-time 40 "$@"; }
post() {
  if [[ -n "${2:-}" ]]; then
    c -X POST -H 'content-type: application/json' "$base$1" -d "$2"
  else
    c -X POST "$base$1"
  fi
}
now() { date +%s.%N; }
log() { echo "[$(date '+%Y-%m-%d %H:%M:%S')] $*" | tee -a "$out/loop.log"; }
fw() { c "$base/api/devices/$position/parameters/0x100A/0" | jq -r '.value // "?"'; }
state() {
  c "$base/api/devices/state?positions=$position" |
    jq -c '.[0] | {alState, alStatusCode, reachability}' 2>/dev/null || echo '{}'
}

netem_off
log "== $label: reset, init, scan, PRE-OP on $adapter"
post /api/reset > /dev/null
post /api/init "{\"adapter\":\"$adapter\"}" | head -c 200
echo
post /api/scan | head -c 200
echo
post /api/devices/state '{"state":2,"timeout":15000}' | head -c 300
echo

echo "round,package,status,seconds,fw_before,fw_after,state_after,error" > "$out/results.csv"
log "== $label: $rounds rounds on device $position, packages $pkg_a / $pkg_b${netem:+, netem: $netem}"
log "device before: fw=$(fw) state=$(state)"

for ((i = 1; i <= rounds; i++)); do
  pkg=$pkg_a
  if ((i % 2 == 0)); then pkg=$pkg_b; fi
  before=$(fw)
  t0=$(now)
  log "round $i: install $pkg (running $before)"
  netem_on
  start=$(post "/api/devices/$position/procedures/firmware-installation" \
    "{\"packageFilename\":\"$pkg\",\"finalState\":2}")
  if [[ $(jq -r '.status // ""' <<<"$start") != running ]]; then
    netem_off
    log "round $i: start refused: $start"
    echo "$i,$pkg,refused,0,$before,,\"$(state)\",\"$(jq -r '.error // .message // ""' <<<"$start")\"" >> "$out/results.csv"
    break
  fi
  while :; do
    sleep 1
    snap=$(c "$base/api/devices/$position/procedures/firmware-installation")
    st=$(jq -r '.status // "poll-failed"' <<<"$snap")
    if [[ $st != running ]]; then break; fi
  done
  t1=$(now)
  netem_off
  secs=$(awk "BEGIN{printf \"%.1f\", $t1 - $t0}")
  echo "$snap" > "$out/round-$i.json"
  err=$(jq -r '.error // ""' <<<"$snap" | tr '"' "'")
  sa=$(state | tr '"' "'")
  after=$(fw)
  echo "$i,$pkg,$st,$secs,$before,$after,\"$sa\",\"$err\"" >> "$out/results.csv"
  log "round $i: $st in ${secs}s, fw $before -> $after, state $sa${err:+, error: $err}"

  if [[ $st != succeeded ]]; then
    log "round $i: recovery: asking for PRE-OP with a 30 s timeout"
    rec=$(post /api/devices/state "{\"positions\":[$position],\"state\":2,\"timeout\":30000}")
    log "round $i: recovery answer: $rec"
    if [[ $(state | jq -r '.alState') != 2 ]]; then
      log "round $i: device did not come back to PRE-OP, stopping. Power-cycle, then scan."
      break
    fi
    log "round $i: recovered, fw=$(fw)"
  fi
done
log "== $label: $(grep -c ',succeeded,' "$out/results.csv") succeeded of $(($(wc -l < "$out/results.csv") - 1)) runs"
