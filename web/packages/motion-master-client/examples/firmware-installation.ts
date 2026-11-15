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
import {
  AlState,
  formatStep,
  HttpError,
  MotionMasterClient,
  type ProcedureSnapshot,
  run,
  stepChanges,
  type TargetAlState,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

const PROCEDURE = 'firmware-installation';

// The states a device can be left in, by their names in lowercase: init, preop, boot, ...
const STATES = Object.fromEntries(
  Object.entries(AlState)
    .filter(([name]) => name !== 'UNKNOWN')
    .map(([name, value]) => [name.toLowerCase(), value]),
) as Record<string, TargetAlState>;

async function main(): Promise<void> {
  const {
    slave,
    package: packagePath,
    finalState: finalStateName,
  } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--package <path>', 'path to the .zip firmware package')
    .addOption(
      new Option('--final-state <state>', 'the state to leave the device in')
        .choices(Object.keys(STATES))
        .default('preop'),
    )
    .parse()
    .opts<{ slave: number; package: string; finalState: string }>();
  const finalState = STATES[finalStateName];
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
    console.log(`could not check the package against the device: ${e.reason}`);
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

  // Ctrl-C aborts the signal, so that runProcedure() cancels the run before the program ends.
  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort(new Error('cancelled')));

  let snapshot: ProcedureSnapshot | undefined;
  try {
    for await (const [current, changed] of stepChanges(
      client.runProcedure(slave, PROCEDURE, { params, signal: controller.signal }),
    )) {
      snapshot = current;
      for (const step of changed) {
        console.log(formatStep(step));
      }
    }
  } catch (e) {
    if (controller.signal.aborted) {
      console.error('\ncancelled');
      process.exitCode = 1;
      return;
    }
    throw e;
  }

  if (snapshot?.error) {
    console.log(`error: ${snapshot.error}`);
  }
  console.log(`${PROCEDURE} ${snapshot?.status}`);
  if (snapshot?.status !== 'succeeded') {
    process.exitCode = 1;
  }
}

await run(main);
