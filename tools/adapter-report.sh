#!/usr/bin/env bash
# Prints the settings of a Linux network adapter and its host that can hold an EtherCAT frame
# before it reaches SOEM or the wire. The output is one block of text to paste into an issue.
# See issue #138, where a PC held a mailbox frame for 100 ms.
#
# The script only reads. It changes no setting and needs no root. A value that needs root to
# read, or that the adapter does not have, shows as "not available".
#
# The summary at the end names what is known to delay frames, and lists the rest without a
# verdict. Two settings are known:
#
# - A tc qdisc rule such as netem holds every frame for the time it names.
# - The cdc_ncm driver holds a transmitted frame for tx_timer_usecs, so that it can put several
#   frames into one USB transfer. It starts the timer again twice (CDC_NCM_TIMER_PENDING_CNT in
#   include/linux/usb/cdc_ncm.h), so one frame waits three times the value. On an ASIX AX88179A
#   the round trip measured 1.3 ms at the default of 400 µs and 0.1 ms at 5 µs, which fits
#   3 * tx_timer_usecs + 0.09 ms. Writing 0 turns the timer off. That was read in the driver
#   source and not measured.
#
# A settings report cannot measure a delay. motion-master-capture measures it.
#
# usage: tools/adapter-report.sh <adapter> [cycle in µs, default 1000]
set -u

adapter="${1:?usage: tools/adapter-report.sh <adapter> [cycle in µs]}"
cycle="${2:-1000}"
net="/sys/class/net/$adapter"
if [[ ! -e "$net" ]]; then
  echo "no adapter named $adapter. Adapters on this host:" >&2
  ls /sys/class/net >&2
  exit 1
fi

section() { printf '\n== %s\n' "$1"; }

# The summary collects its lines while the sections print.
known=()
check=()

# Runs a command and prints its output, or "not available" when the command is missing or fails.
run() {
  local output
  if ! command -v "$1" > /dev/null; then
    echo "not available: $1 is not installed"
    return
  fi
  if output="$("$@" 2>&1)"; then
    echo "$output"
  else
    echo "not available: $* failed: $output"
  fi
}

# Prints "name = value" for each readable file in the arguments.
values() {
  local file
  for file in "$@"; do
    if [[ -f "$file" && -r "$file" ]]; then
      printf '%s = %s\n' "$file" "$(tr '\n' ' ' < "$file" 2>/dev/null)"
    elif [[ -e "$file" ]]; then
      printf '%s = not available\n' "$file"
    fi
  done
}

device="$(readlink -f "$net/device" 2>/dev/null || true)"
driver="$(basename "$(readlink -f "$net/device/driver" 2>/dev/null)" 2>/dev/null || true)"

section "Host"
run uname -r
values /sys/kernel/realtime /proc/cmdline
# systemd-detect-virt exits non-zero when it prints "none".
echo "virtualization: $(systemd-detect-virt 2>/dev/null || true)"
values /sys/devices/system/cpu/cpuidle/current_driver \
  /sys/devices/system/cpu/cpuidle/current_governor \
  /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor \
  /sys/firmware/acpi/platform_profile
if command -v powerprofilesctl > /dev/null; then
  run powerprofilesctl get
fi
if command -v tuned-adm > /dev/null; then
  run tuned-adm active
fi

section "Adapter"
run ip -d link show dev "$adapter"
run ethtool -i "$adapter"
# Without root, ethtool cannot read the Wake-on-LAN settings and prints a netlink error for it.
run ethtool "$adapter" | grep -v "netlink error"
echo "device: ${device:-none}"

section "Queueing discipline (a netem rule here delays every frame)"
run tc -s qdisc show dev "$adapter"
values "$net/tx_queue_len"

section "Interrupt coalescing"
run ethtool -c "$adapter"
if [[ "$driver" == r8152 ]]; then
  # r8152_get_coalesce copies tp->coalesce, which is in nanoseconds, into rx_coalesce_usecs.
  echo "note: the r8152 driver reports rx-usecs in nanoseconds. 15000 is 15 µs."
fi
values "$net/gro_flush_timeout" "$net/napi_defer_hard_irqs" "$net/threaded"

section "Offloads that can change"
run ethtool -k "$adapter" | grep -v "\[fixed\]"

section "Ring sizes"
run ethtool -g "$adapter"

section "Energy Efficient Ethernet"
run ethtool --show-eee "$adapter"

section "Driver module parameters"
if [[ -n "$driver" && -d "/sys/module/$driver/parameters" ]]; then
  values "/sys/module/$driver/parameters/"*
else
  echo "none for driver ${driver:-unknown}"
fi

section "cdc_ncm (frames wait up to tx_timer_usecs to fill one USB transfer)"
if [[ -d "$net/cdc_ncm" ]]; then
  values "$net/cdc_ncm/"*
  timer="$(cat "$net/cdc_ncm/tx_timer_usecs" 2>/dev/null || echo "")"
  if [[ "$timer" =~ ^[0-9]+$ ]]; then
    if ((timer == 0)); then
      known+=("cdc_ncm: tx_timer_usecs is 0, so the driver sends each frame at once.")
    else
      roundtrip=$((3 * timer + 90))
      line="cdc_ncm: tx_timer_usecs is $timer, so a frame round trip is about $roundtrip µs."
      if ((2 * roundtrip > cycle)); then
        line+=" That is more than half of the $cycle µs cycle. LATE FRAMES ARE EXPECTED."
        line+=" To set it until the adapter is plugged in again:"
        line+=" echo 5 | sudo tee $net/cdc_ncm/tx_timer_usecs."
        line+=" To keep it, put this line in /etc/udev/rules.d/70-cdc-ncm-tx-timer.rules and run"
        line+=" sudo udevadm control --reload:"
        line+=' ACTION=="add", SUBSYSTEM=="net", DRIVERS=="cdc_ncm", ATTR{cdc_ncm/tx_timer_usecs}="5"'
      fi
      known+=("$line")
    fi
  fi
else
  echo "not a cdc_ncm adapter"
fi

# A USB adapter's network device sits on a USB interface. The directories above it are the USB
# device and then each hub up to the root. Each one has its own speed and power management.
if [[ "$device" == */usb* ]]; then
  section "USB path, from the adapter to the root hub"
  values /sys/module/usbcore/parameters/autosuspend
  dir="$(dirname "$device")"
  while [[ -f "$dir/idVendor" ]]; do
    echo "-- $dir"
    values "$dir/idVendor" "$dir/idProduct" "$dir/manufacturer" "$dir/product" "$dir/version" \
      "$dir/speed" "$dir/power/control" "$dir/power/autosuspend_delay_ms" \
      "$dir/power/runtime_status" "$dir/power/usb2_hardware_lpm" \
      "$dir/power/usb3_hardware_lpm_u1" "$dir/power/usb3_hardware_lpm_u2"
    dir="$(dirname "$dir")"
  done
  if command -v lsusb > /dev/null; then
    run lsusb -t
  fi
elif [[ -f "$device/vendor" ]]; then
  section "PCI device"
  values "$device/vendor" "$device/device" "$device/current_link_speed" \
    "$device/current_link_width" "$device/power/control" "$device/power/runtime_status" \
    "$device/link/"* /sys/module/pcie_aspm/parameters/policy
fi

section "Interrupts"
if [[ -n "$driver" ]]; then
  grep -E "$adapter|$driver|xhci" /proc/interrupts || echo "no interrupt line names $adapter"
fi

section "NetworkManager"
if command -v nmcli > /dev/null; then
  run nmcli -f GENERAL.STATE,GENERAL.CONNECTION,IP4.ADDRESS device show "$adapter"
else
  echo "not installed"
fi

section "Packet sockets (another program on the adapter adds traffic)"
run ss -0 -a -p

section "Kernel messages about the adapter"
# dmesg needs root on many distributions. journalctl -k needs only the systemd-journal group.
if output="$(dmesg 2>/dev/null)" || output="$(journalctl -k -b --no-pager 2>/dev/null)"; then
  grep -iE "$adapter|${driver:-$adapter}" <<< "$output" | tail -n 40
else
  echo "not available: dmesg and journalctl -k need root on this host"
fi

# The checks below read the values again, so that each one is in one place.
qdiscs="$(tc qdisc show dev "$adapter" 2>/dev/null)"
if grep -q netem <<< "$qdiscs"; then
  known+=("qdisc: a netem rule is on the adapter. It delays frames by what it names: $(grep netem <<< "$qdiscs")")
elif grep -qvE "^qdisc (fq_codel|pfifo_fast|noqueue|mq|fq) " <<< "$qdiscs"; then
  check+=("qdisc: a qdisc that is not a distribution default is on the adapter: $qdiscs")
fi

operstate="$(cat "$net/operstate" 2>/dev/null)"
if [[ "$operstate" != up ]]; then
  known+=("link: the adapter is $operstate. Connect the bus and run the script again.")
else
  speed="$(cat "$net/speed" 2>/dev/null)"
  duplex="$(cat "$net/duplex" 2>/dev/null)"
  # EtherCAT runs at 100 Mb/s full duplex.
  if [[ "$duplex" != full || "$speed" != 100 ]]; then
    known+=("link: $speed Mb/s $duplex duplex. EtherCAT needs 100 Mb/s full duplex.")
  fi
fi

if [[ "$device" == */usb* ]]; then
  usb="$(dirname "$device")"
  usbspeed="$(cat "$usb/speed" 2>/dev/null)"
  if [[ "$usbspeed" =~ ^[0-9]+$ ]] && ((usbspeed < 5000)); then
    check+=("usb: the adapter runs at $usbspeed Mb/s, not at USB 3 speed.")
  fi
  if [[ "$(cat "$usb/power/control" 2>/dev/null)" == auto ]]; then
    check+=("usb: autosuspend is allowed for the adapter (power/control is auto).")
  fi
fi

eee="$(ethtool --show-eee "$adapter" 2>/dev/null | awk -F': ' '/EEE status/ {print $2}')"
if [[ "$eee" == *" - active"* ]]; then
  check+=("eee: Energy Efficient Ethernet is active on the link.")
fi
if [[ "$(cat "$net/gro_flush_timeout" 2>/dev/null)" != 0 ]]; then
  check+=("napi: gro_flush_timeout is $(cat "$net/gro_flush_timeout" 2>/dev/null), not 0.")
fi
governor="$(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null)"
if [[ -n "$governor" && "$governor" != performance ]]; then
  check+=("cpu: the frequency governor is $governor, not performance.")
fi

section "Summary"
echo "Known to delay frames, or known to be wrong for EtherCAT:"
if ((${#known[@]} == 0)); then
  echo "  nothing found"
fi
for line in "${known[@]}"; do
  echo "  - $line"
done
echo "Not known to delay frames. Compare these with a host that has no late frames:"
if ((${#check[@]} == 0)); then
  echo "  nothing found"
fi
for line in "${check[@]}"; do
  echo "  - $line"
done
echo "This report reads settings only. motion-master-capture measures the delay of each frame."
