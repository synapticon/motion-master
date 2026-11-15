// Blink the LEDs of a device, so that someone at the machine can find it (device-locate).
//
// The LEDs blink for --duration seconds and stop at the end of the run, or at once on Ctrl-C. Only a
// fieldbus driver that can blink the LEDs offers the procedure, which today is SPoE. Over SOEM the
// device has no device-locate procedure, and the server refuses the run with 404.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/device-locate.ts
//     pnpm example examples/device-locate.ts --duration 30 --slave 2

import {
  AlState,
  MotionMasterClient,
  type ProcedureRequest,
  run,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';
import { runToEnd } from './procedures';

const PROCEDURE = 'device-locate';

async function main(): Promise<void> {
  const { slave, duration } = cli()
    .description(
      'Blink the LEDs of a device, so that someone at the machine can find it (device-locate).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--duration <s>', 'seconds, 1 to 3600', integer, 10)
    .parse()
    .opts<{ slave: number; duration: number }>();
  const params = { duration } as ProcedureRequest;

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  await runToEnd(client, slave, PROCEDURE, params);
}

await run(main);
