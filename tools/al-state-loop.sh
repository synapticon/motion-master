#!/usr/bin/env bash
# Walks one drive INIT, PRE-OP, SAFE-OP, OP, INIT for N rounds against a running Motion Master,
# with a netem rule on the bus adapter during the rounds, and counts in the server log how often
# the step to SAFE-OP mapped the process data short or had to map again.
#
# One run is: reset, init, scan, PRE-OP, then N rounds of the five state requests. The netem rule
# is removed for the bring-up, added for the rounds, and removed at the end. See NEXTGEN.md,
# Session 2026-10-22, for what each count should show.
#
# Recipes, see NEXTGEN.md, Session 2026-10-20:
#   NETEM="delay 150ms reorder 98% 0%"   2 % of the frames 150 ms late
#   NETEM="loss 1%"                      1 % of the frames lost before the slave sees them
#   NETEM=""                             no rule, the baseline
#
# tc needs root. One line in /etc/sudoers.d/ lets this script switch the rule without a password:
#   marko ALL=(root) NOPASSWD: /usr/sbin/tc
#
# The server log rotates at 10 MB. The counts come from the lines written during the rounds, and a
# rotation during a run leaves them at zero.
#
# usage: NETEM="<netem parameters>" tools/al-state-loop.sh <label> [rounds] [adapter]
#
# Results go to out/al-state-loop/<label>/: the server log lines written during the rounds.
set -u

label="${1:?usage: NETEM=\"...\" tools/al-state-loop.sh <label> [rounds] [adapter]}"
rounds="${2:-30}"
adapter="${3:-enp0s13f0u1u4u2}"
netem="${NETEM:-}"
base="${MM_BASE_URL:-https://local.motion-master.synapticon.com:61447}"
log="${MM_LOG:-$HOME/.cache/motion-master/logs/motion-master.log}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$repo/out/al-state-loop/$label"
mkdir -p "$out"

netem_off() { sudo tc qdisc del dev "$adapter" root 2>/dev/null; }
netem_on() {
  if [[ -n "$netem" ]]; then
    # shellcheck disable=SC2086  # the parameters are several words on purpose
    sudo tc qdisc add dev "$adapter" root netem $netem
  fi
}

post() {
  if [[ -n "${2:-}" ]]; then
    curl -sk --max-time 120 -w ' [%{http_code}]' -X POST -H 'content-type: application/json' \
      "$base$1" -d "$2"
  else
    curl -sk --max-time 120 -w ' [%{http_code}]' -X POST "$base$1"
  fi
}

netem_off
echo "== $label: reset, init, scan, PRE-OP"
post /api/reset > /dev/null
post /api/init "{\"adapter\":\"$adapter\"}" | head -c 200
echo
post /api/scan | head -c 200
echo
post /api/devices/state '{"state":2,"timeoutMs":15000}' | head -c 300
echo

start_line=$(wc -l < "$log")
echo "== $label: $rounds rounds, netem '$netem' (log from line $start_line)"
fails=0
for ((i = 1; i <= rounds; i++)); do
  netem_on
  line="round $(printf %2d "$i"):"
  for s in 1 2 4 8 1; do
    t0=$(date +%s.%N)
    r=$(post /api/devices/state "{\"state\":$s,\"timeoutMs\":15000}")
    t1=$(date +%s.%N)
    code="${r##*\[}"
    code="${code%]}"
    if [[ "$code" != "200" ]]; then
      fails=$((fails + 1))
      echo "$line $s FAILED: $r"
    fi
    line="$line $s:$code/$(printf %.1f "$(echo "$t1 - $t0" | bc)")s"
  done
  netem_off
  configured=$(tail -n +"$((start_line + 1))" "$log" |
    grep -o 'Process data configured: [0-9]* output bytes, [0-9]* input bytes' | tail -1)
  echo "$line | $configured"
done
netem_off

tail -n +"$((start_line + 1))" "$log" > "$out/server.log"
count() { grep -c "$1" "$out/server.log"; }
echo "== $label: server log during the rounds"
printf 'failed state requests:         %s\n' "$fails"
printf 'map attempts tried again:      %s\n' "$(count 'Mapping again')"
printf 'maps refused as short:         %s\n' "$(count 'so the map is short')"
printf 'SOEM maps:                     %s\n' "$(count 'Process data mapped')"
printf 'SOEM maps with out 0 or in 0:  %s\n' \
  "$(grep -c 'Process data mapped: [0-9]* bytes (out 0\|Process data mapped: [0-9]* bytes (out [0-9]*, in 0)' "$out/server.log")"
printf 'images published:              %s\n' "$(count 'Process data configured')"
printf 'images with 0 output bytes:    %s\n' "$(count 'Process data configured: 0 output')"
printf 'writes without a frame back:   %s\n' \
  "$(grep -o '[0-9]* mailbox write(s) got no frame back' "$out/server.log" | awk '{s+=$1} END {print s+0}')"
printf 'requests sent with counter 0:  %s\n' \
  "$(grep -o '[0-9]* request(s) went out with mailbox counter 0' "$out/server.log" | awk '{s+=$1} END {print s+0}')"
printf 'no response (mailbox timeout): %s\n' "$(count 'no response')"
printf 'no frame returned:             %s\n' "$(count 'no frame returned')"
printf 'warnings / errors:             %s / %s\n' "$(count '\[warning\]')" "$(count '\[error\]')"
