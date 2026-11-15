// Delete a file from the flash of a SOMANET device (DELETE .../files/{filename}).
//
// The file is gone from flash at once. A file that was not there also succeeds, because the server
// answers that the file is gone. The device answers only after it erased the file, which can take
// seconds. Deleting config.csv discards the stored parameter values, so the drive starts with its
// defaults after the next power cycle. A device that is not a SOMANET device is refused.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP.
//
//     pnpm example examples/delete-file.ts --filename notes.txt
//     pnpm example examples/delete-file.ts --filename hr_data0.bin --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

// The server does not URL-decode a path parameter, so the program keeps = as it is: a name such as
// fs-stackunlock=DD1317 must reach the device unchanged.
function encodeFilename(name: string): string {
  return encodeURIComponent(name).replace(/%3D/g, '=');
}

async function main(): Promise<void> {
  const { slave, filename } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--filename <name>', 'the name of the file on the device')
    .parse()
    .opts<{ slave: number; filename: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  await client.api.deleteDeviceFile(slave, encodeFilename(filename));
  console.log(`slave ${slave}:${filename} is gone`);
}

await run(main);
