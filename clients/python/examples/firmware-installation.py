#!/usr/bin/env python3
"""Install a SOMANET firmware package on a device (firmware-installation).

The script uploads the package into the firmwares/ directory of the user cache of the server
(PUT /api/user-cache/firmwares/<name>), and starts the procedure with only the filename, so the
package moves as raw bytes. The procedure takes the device to BOOT, writes the files of the
package over FoE, and moves the device to --final-state. PREOP, the default, is the proof that
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

import logging
import sys
from pathlib import Path

import requests

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from motion_master_client import (  # noqa: E402
    MotionMasterClient,
    error_line,
    run,
)
from options import AL_STATES, integer, parser  # noqa: E402
from procedures import run_to_end  # noqa: E402

PROCEDURE = "firmware-installation"


def main():
    ap = parser(__doc__)
    ap.add_argument(
        "--slave", type=integer, default=1, help="the position of the device on the bus"
    )
    ap.add_argument("--package", required=True, help="path to the .zip firmware package")
    ap.add_argument(
        "--final-state",
        choices=AL_STATES,
        default="preop",
        help="the state to leave the device in",
    )
    args = ap.parse_args()

    package = Path(args.package)
    logging.basicConfig(level=logging.INFO, format="%(message)s")
    client = MotionMasterClient.from_config()
    client.init()
    client.ensure_scanned()

    # The check reads two files of the device over FoE, so it fails on a device with no mailbox,
    # such as one in INIT. The install goes ahead without it.
    try:
        result = client.call(
            "checkDeviceFirmwareCompatibility",
            path={"slavePosition": args.slave},
            query={"filename": package.name},
        ).json()
        verdict = "fits" if result["compatible"] else "DOES NOT FIT"
        print(f"package {verdict} the device: {result['explanation']}")
    except requests.HTTPError as error:
        print(f"could not check the package against the device: {error_line(error)}")

    client.write_user_cache_file(f"firmwares/{package.name}", package.read_bytes())
    print(f"uploaded {package.name}")
    body = {"packageFilename": package.name, "finalState": AL_STATES[args.final_state]}

    run_to_end(client, args.slave, PROCEDURE, body)


if __name__ == "__main__":
    run(main)
