#!/usr/bin/env python3
"""Install a SOMANET firmware package on a device (firmware-installation).

The script uploads the package into the firmwares/ directory of the user cache of the server
(PUT /api/user-cache/firmwares/<name>), and starts the procedure with only the filename, so the
package moves as raw bytes. The procedure takes the device to BOOT, writes the files of the
package over FoE, and moves the device to --final-state. PRE-OP, the default, is the proof that
the install worked: the bootloader starts the new firmware on that transition.

The device exchanges no process data for the whole installation, so whatever it drives is not
controlled from the moment it leaves OP. Ctrl-C between two files leaves the device with part of
the firmware written. A package that writes an SII needs a power cycle for the SII to apply.

Before the install, the script reports whether the package fits the device, as
check-firmware-compatibility.py does. It installs a package that does not fit all the same,
because the procedure writes whatever it gets.

Opens the fieldbus driver and scans first when needed, and moves no device. The procedure moves
the device itself, also from BOOT.

    python firmware-installation.py --package package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip
    python firmware-installation.py --package firmware.zip --final-state boot --slave 2
"""

import argparse
import logging
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import (  # noqa: E402
    STATE_NAMES,
    MotionMasterClient,
    ProcedureError,
    format_step,
    step_changes,
)

PROCEDURE = "firmware-installation"
STATES = {name.lower(): value for value, name in STATE_NAMES.items() if value != 0}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--slave", type=int, default=1)
    ap.add_argument("--package", required=True, help="path to the .zip firmware package")
    ap.add_argument("--final-state", choices=STATES, default="pre-op")
    args = ap.parse_args()

    package = Path(args.package)
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    # The check reads two files of the device over FoE, so it fails on a device with no mailbox,
    # such as one in INIT. The install goes ahead without it.
    r = client.call(
        "checkDeviceFirmwareCompatibility",
        path={"slavePosition": args.slave},
        query={"filename": package.name},
        check=False,
    )
    if r.status_code == 200:
        result = r.json()
        verdict = "fits" if result["compatible"] else "DOES NOT FIT"
        print(f"package {verdict} the device: {result['explanation']}")
    else:
        print(f"could not check the package against the device: {r.json().get('error', r.text)}")

    client.call(
        "putUserCacheFile",
        path={"path": f"firmwares/{package.name}"},
        data=package.read_bytes(),
        headers={"Content-Type": "application/octet-stream"},
    )
    print(f"uploaded {package.name}")
    body = {"packageFilename": package.name, "finalState": STATES[args.final_state]}

    snapshot = None
    changes = step_changes(client.run_procedure(args.slave, PROCEDURE, body))
    try:
        for snapshot, changed in changes:
            for step in changed:
                print(format_step(step))
    except ProcedureError as e:
        sys.exit(str(e))
    except KeyboardInterrupt:
        print("\ncancelled")
        sys.exit(1)
    finally:
        # Closing the generator cancels a run that is still in progress.
        changes.close()

    if snapshot.get("error"):
        print(f"error: {snapshot['error']}")
    print(f"{PROCEDURE} {snapshot['status']}")
    if snapshot["status"] != "succeeded":
        sys.exit(1)


if __name__ == "__main__":
    main()
