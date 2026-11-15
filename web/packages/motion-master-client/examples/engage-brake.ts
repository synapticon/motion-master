// Engage the brake of a SOMANET drive (POST .../brake/engage).
//
// The server writes "engaged" to the brake state, 0x2004:07, and waits --settle. A brake engages by
// spring force when its voltage goes, so there is no pull time to wait for. A brake that the drive
// does not let the master control is left alone, and the state says so. The program prints the
// brake state that the server reads back.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/engage-brake.ts
//     pnpm example examples/engage-brake.ts --settle 200 --slave 2

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

  const state = (await client.api.engageBrake(slave, { settle })).data;
  console.log(JSON.stringify(state, null, 2));
}

await run(main);
