#!/usr/bin/env python3
"""Record a velocity move with a monitoring, print it, and save every cycle as CSV.

The motor of the drive turns. Make sure it can turn freely before you run this.

The script brings the drive to OP and starts a monitoring of seven objects: Timestamp (0x20F0),
Statusword (0x6041), Modes of operation display (0x6061), Target velocity (0x60FF), Velocity actual
value (0x606C), Position actual value (0x6064) and Torque actual value (0x6077). While the monitoring records, it
selects Cyclic Synchronous Velocity (CSV, mode 9), enables the drive, sets the target velocity,
holds it for --seconds, sets it back to 0, and disables the drive.

In CSV the drive takes the target velocity of every cycle as it is, with no profile ramp of its
own, so the target steps from 0 to --velocity and back. The velocity is in the units of 0x60FF.

client.monitoring() records every cycle while the with block runs, also the cycles between two
flushes: an object in the process data gives one value per cycle. After the move, the Monitoring
shows its rows: m.print_table() prints one row every --every ms, with the CiA402 state beside the
statusword, and m.write_csv() writes every row to a CSV file. Both name a column after the device
position and the object, such as 1:VELOCITY_ACTUAL_VALUE. The script also prints how fast the
velocity reached the target and how close it held it, from m.first() and m.values().

Two clocks are in each row. The row timestamp is the time on the server when the cycle ran.
Timestamp (0x20F0) is the clock of the drive, which it sends in the process data. It wraps, so a
drop in it is not a restart of the drive. Use the row timestamp for the time of a cycle.

    python monitoring.py
    python monitoring.py --slave 2 --velocity 500 --seconds 3 --output /tmp/move.csv
"""

import argparse
import contextlib
import logging
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import AlState, Cia402, MotionMasterClient, Somanet, run  # noqa: E402

FILES_DIR = Path(__file__).resolve().parent.parent / "files"

CYCLIC_SYNC_VELOCITY = 9

# The monitored objects, in the order of the columns.
OBJECTS = [
    Somanet.TIMESTAMP,
    Cia402.STATUSWORD,
    Cia402.MODES_OF_OPERATION_DISPLAY,
    Cia402.TARGET_VELOCITY,
    Cia402.VELOCITY_ACTUAL_VALUE,
    Cia402.POSITION_ACTUAL_VALUE,
    Cia402.TORQUE_ACTUAL_VALUE,
]

# How long the recording goes on after the target is back at 0, so that it holds the stop.
AFTER_STOP_SECONDS = 1.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--velocity", type=int, default=250, help="target velocity, units of 0x60FF")
    ap.add_argument("--seconds", type=float, default=2.0, help="how long to hold the velocity")
    ap.add_argument("--every", type=int, default=100, help="print one line every N ms")
    ap.add_argument("--output", help="CSV file (default: files/monitoring-<time>.csv)")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_state(AlState.OP, positions=[args.slave])

    parameters = [(args.slave, *address) for address in OBJECTS]
    try:
        with client.monitoring(parameters, interval_ms=50) as m:
            client.set_operation_mode(args.slave, CYCLIC_SYNC_VELOCITY)
            # A target left from an earlier run would turn the motor at the enable.
            client.set_target(args.slave, "velocity", 0)
            client.enable(args.slave)
            print(f"turning at {args.velocity} for {args.seconds} s")
            client.set_target(args.slave, "velocity", args.velocity)
            time.sleep(args.seconds)
            client.set_target(args.slave, "velocity", 0)
            time.sleep(AFTER_STOP_SECONDS)
    finally:
        # Never leave the drive enabled, also when a step above failed or Ctrl-C stopped it.
        with contextlib.suppress(Exception):
            client.disable(args.slave)

    for name, source in zip(m.names, m.sources):
        if source != "pdo":
            print(f"note: {name} is not in the process data, so it has one value per flush")

    m.print_table(every_ms=args.every)
    print_summary(m, args.slave, args.velocity)

    if args.output:
        out = Path(args.output)
    else:
        out = FILES_DIR / f"monitoring-{datetime.now():%Y%m%d-%H%M%S}.csv"
    m.write_csv(out)
    print(f"saved {len(m.rows)} row(s) -> {out}")


def print_summary(m, slave, velocity):
    """Print how long the actual velocity took to reach the target, and how close it held it."""
    t_target = m.first(slave, Cia402.TARGET_VELOCITY, lambda v: v == velocity)
    if t_target is None:
        print(f"the target velocity never read {velocity}")
        return
    t_off = m.first(slave, Cia402.TARGET_VELOCITY, lambda v: v != velocity, after=t_target)
    held = m.values(slave, Cia402.VELOCITY_ACTUAL_VALUE, start=t_target, end=t_off)
    # The second half of the hold, where the step response has settled.
    settled = held[len(held) // 2 :]
    if not settled:
        print("no velocity was recorded while the target was held")
        return
    mean = sum(settled) / len(settled)
    t_reached = m.first(
        slave, Cia402.VELOCITY_ACTUAL_VALUE, lambda v: v >= velocity * 0.95, after=t_target
    )
    reached = f"{(t_reached - t_target) / 1000:.0f} ms" if t_reached is not None else "never"
    print(
        f"{len(m.rows)} cycles over {(m.rows[-1][0] - m.rows[0][0]) / 1e6:.2f} s; "
        f"95 % of the target after {reached}; second half of the hold: mean {mean:.1f}, "
        f"min {min(settled)}, max {max(settled)} over {len(settled)} cycles"
    )


if __name__ == "__main__":
    run(main)
