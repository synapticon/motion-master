// Release the brake of a SOMANET drive (POST .../brake/release).
//
// The server writes "disengaged" to the brake state, 0x2004:07, and waits the pull time of the
// drive, 0x2004:03, plus --settle. The brake releases only while the drive is in Operation Enabled.
// In any other state the write only energises the brake output. A released brake stays released, so
// on a vertical or loaded axis support the load, and engage the brake again with engage-brake.ts. On
// a pin brake the release turns the shaft to lift the load off the pin. A brake that the drive does
// not let the master control is left alone, and the state says so. The program prints the brake
// state that the server reads back.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/release-brake.ts
//     pnpm example examples/release-brake.ts --settle 200 --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, settle } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--settle <ms>', 'extra wait in ms', integer, 50)
    .parse()
    .opts<{ slave: number; settle: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const state = (await client.api.releaseBrake(slave, { settle })).data;
  console.log(JSON.stringify(state, null, 2));
}

await run(main);
