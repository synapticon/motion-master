#!/usr/bin/env bash
# Runs a mailbox workload against a running Motion Master while netem makes the bus adapter drop
# or delay frames. It counts the mailbox warnings and the failed requests in the server log.
#
# One run is: reset, init, scan, PRE-OP, then N rounds of a full parameter read
# (POST /api/devices/<position>/parameters/read) and one FoE file read
# (GET /api/devices/<position>/files/<file>). Each file is compared with the first round's bytes.
#
# The netem rule is removed for reset, init, scan and PRE-OP, added for the rounds, and removed at
# the end. A uniform delay during a scan makes the state writes fail, because SOEM waits 6 ms for one.
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
# rotation during a run leaves them at zero. A zero count of "SDO abort" on a SOMANET drive, which
# refuses its write-only objects on every full read, shows that this happened.
#
# usage: NETEM="<netem parameters>" tools/flaky-bus.sh <label> [rounds] [adapter] [position] [file]
#
# Results go to out/flaky-bus/<label>/: the parameter lists, the files, and the server log lines.
set -u

label="${1:?usage: NETEM=\"...\" tools/flaky-bus.sh <label> [rounds] [adapter] [position] [file]}"
rounds="${2:-10}"
adapter="${3:-enp0s13f0u1u4u2}"
position="${4:-1}"
file="${5:-config.csv}"
netem="${NETEM:-}"
base="${MM_BASE_URL:-https://local.motion-master.synapticon.com:61447}"
log="${MM_LOG:-$HOME/.cache/motion-master/logs/motion-master.log}"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
out="$repo/out/flaky-bus/$label"
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
    curl -sk --max-time 120 -X POST -H 'content-type: application/json' "$base$1" -d "$2"
  else
    curl -sk --max-time 120 -X POST "$base$1"
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
sleep 1

post /api/frames/reset > /dev/null
netem_on
echo "== $label: netem: $(tc qdisc show dev "$adapter" | head -1)"
start_line=$(wc -l < "$log")
echo "== $label: $rounds rounds (log from line $start_line)"
first_md5=""
for ((i = 1; i <= rounds; i++)); do
  t0=$(date +%s.%N)
  status=$(curl -sk --max-time 120 -o "$out/params-$i.json" -w '%{http_code}' -X POST \
    "$base/api/devices/$position/parameters/read")
  t1=$(date +%s.%N)
  fstatus=$(curl -sk --max-time 60 -o "$out/file-$i.bin" -w '%{http_code}' \
    "$base/api/devices/$position/files/$file")
  t2=$(date +%s.%N)
  md5=$(md5sum "$out/file-$i.bin" | cut -c1-8)
  [[ -z "$first_md5" ]] && first_md5="$md5"
  same=$([[ "$md5" == "$first_md5" ]] && echo same || echo DIFFERENT)
  stale=$(grep -o '"state":"stale"' "$out/params-$i.json" | wc -l)
  printf 'round %2d: read %s %.2fs (stale %d) | file %s %.2fs %s bytes %s\n' "$i" "$status" \
    "$(echo "$t1 - $t0" | bc)" "$stale" "$fstatus" "$(echo "$t2 - $t1" | bc)" \
    "$(stat -c %s "$out/file-$i.bin")" "$same"
done
netem_off

tail -n +"$((start_line + 1))" "$log" > "$out/server.log"
count() { grep -c "$1" "$out/server.log"; }
echo "== $label: server log during the rounds"
printf 'writes without a frame back:   %s\n' \
  "$(grep -o '[0-9]* mailbox write(s) got no frame back' "$out/server.log" | awk '{s+=$1} END {print s+0}')"
printf 'requests sent with counter 0:  %s\n' \
  "$(grep -o '[0-9]* request(s) went out with mailbox counter 0' "$out/server.log" | awk '{s+=$1} END {print s+0}')"
printf 'no response (mailbox timeout): %s\n' "$(count 'no response')"
printf 'no frame returned:             %s\n' "$(count 'no frame returned')"
printf 'packet error:                  %s\n' "$(count 'packet/timeout error')"
printf 'SDO abort:                     %s\n' "$(count 'SDO abort')"
printf 'FoE failures:                  %s\n' "$(grep -ci 'FOE.*failed' "$out/server.log")"
printf 'warnings / errors:             %s / %s\n' "$(count '\[warning\]')" "$(count '\[error\]')"

echo "== $label: frames of this host during the rounds (GET /api/frames)"
curl -sk "$base/api/frames"
echo
