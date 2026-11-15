// Install a SOMANET firmware package on a device (firmware-installation).
//
// The program uploads the package into the firmwares/ directory of the user cache of the server
// (PUT /api/user-cache/firmwares/<name>), and starts the procedure with only the filename, so the
// package moves as raw bytes. The procedure takes the device to BOOT, writes the files of the
// package over FoE, and moves the device to --final-state. PREOP, the default, is the proof that
// the install worked: the bootloader starts the new firmware on that transition.
//
// The device exchanges no process data for the whole installation, so whatever it drives is not
// controlled from the moment it leaves OP. Ctrl-C between two files leaves the device with part of
// the firmware written. A package that writes an SII needs a power cycle for the SII to apply.
//
// Before the install, the program reports whether the package fits the device, as
// check-firmware-compatibility.ts does. It installs a package that does not fit all the same,
// because the procedure writes whatever it gets.
//
// Opens the fieldbus driver and scans first when needed, and moves no device. The procedure moves
// the device itself, also from BOOT.
//
//     pnpm example examples/firmware-installation.ts --package package_SOMANET-Circulo-7_8500-04-2332_motion-drive_v5.6.10.zip
//     pnpm example examples/firmware-installation.ts --package firmware.zip --final-state boot --slave 2

import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { HttpError, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { AL_STATES, cli, integer } from './options';
import { runToEnd } from './procedures';

const PROCEDURE = 'firmware-installation';

async function main(): Promise<void> {
  const {
    slave,
    package: packagePath,
    finalState: finalStateName,
  } = cli()
    .description('Install a SOMANET firmware package on a device (firmware-installation).')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--package <path>', 'path to the .zip firmware package')
    .addOption(
      new Option('--final-state <state>', 'the state to leave the device in')
        .choices(Object.keys(AL_STATES))
        .default('preop'),
    )
    .parse()
    .opts<{ slave: number; package: string; finalState: string }>();
  const finalState = AL_STATES[finalStateName];
  const packageName = basename(packagePath);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  // The check reads two files of the device over FoE, so it fails on a device with no mailbox,
  // such as one in INIT. The install goes ahead without it.
  try {
    const result = (
      await client.api.checkDeviceFirmwareCompatibility(slave, { filename: packageName })
    ).data;
    const verdict = result.compatible ? 'fits' : 'DOES NOT FIT';
    console.log(`package ${verdict} the device: ${result.explanation}`);
  } catch (e) {
    if (!(e instanceof HttpError)) {
      throw e;
    }
    console.log(`could not check the package against the device: ${e.message}`);
  }

  // The generated putUserCacheFile encodes its body as JSON, so the raw bytes go with fetch.
  const url = `${client.api.baseUrl}/api/user-cache/firmwares/${encodeURIComponent(packageName)}`;
  const response = await fetch(url, {
    method: 'PUT',
    headers: { 'Content-Type': 'application/octet-stream' },
    body: readFileSync(packagePath),
  });
  if (!response.ok) {
    throw await HttpError.from(new Request(url, { method: 'PUT' }), response);
  }
  console.log(`uploaded ${packageName}`);
  const params = { packageFilename: packageName, finalState };

  await runToEnd(client, slave, PROCEDURE, params);
}

await run(main);
