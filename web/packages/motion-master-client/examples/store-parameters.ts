// Store the current parameter values of a device in its flash (procedure store-parameters).
//
// The procedure writes the "save" signature to 0x1010 and polls the device until it reports the
// store complete, which takes about a second. It stores every value the device holds, so it also
// keeps every change written since the last store. Without a store, a power cycle loses them.
// Ctrl-C stops the wait, not the store, because the device already has the command.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/store-parameters.ts
//     pnpm example examples/store-parameters.ts --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';
import { runToEnd } from './procedures';

const PROCEDURE = 'store-parameters';

async function main(): Promise<void> {
  const { slave } = cli()
    .description(
      'Store the current parameter values of a device in its flash (procedure store-parameters).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  await runToEnd(client, slave, PROCEDURE);
}

await run(main);
