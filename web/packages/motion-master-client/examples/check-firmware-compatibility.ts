// Check whether a firmware package belongs on a device (GET .../firmware-compatibility).
//
// The server reads the .hardware_description and .variant files of the device, and compares the
// firmware descriptors the device accepts with the descriptor in the package filename. The program
// needs only the filename, not the package. It installs nothing, and the firmware installation
// procedure does not check the answer. The program exits with 1 when the package does not belong on
// the device.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP.
//
//     pnpm example examples/check-firmware-compatibility.ts --filename package_ACTILINK_6000-05-2423-1_motion-drive_v5.6.11.zip

import { basename } from 'node:path';
import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, filename } = cli()
    .description(
      'Check whether a firmware package belongs on a device (GET .../firmware-compatibility).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--filename <name>', 'the package filename, a path is allowed')
    .parse()
    .opts<{ slave: number; filename: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  // A 4xx means that the server could not ask the question, for example because the filename is
  // not a package name. run() prints its reason.
  const result = (
    await client.api.checkDeviceFirmwareCompatibility(slave, { filename: basename(filename) })
  ).data;
  console.log(`${result.compatible ? 'compatible' : 'not compatible'}: ${result.explanation}`);
  if (!result.compatible) {
    process.exitCode = 1;
  }
}

await run(main);
