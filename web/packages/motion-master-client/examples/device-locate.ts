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
  formatStep,
  MotionMasterClient,
  type ProcedureRequest,
  type ProcedureSnapshot,
  run,
  stepChanges,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';

const PROCEDURE = 'device-locate';

async function main(): Promise<void> {
  const { slave, duration } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--duration <s>', 'seconds, 1 to 3600', integer, 10)
    .parse()
    .opts<{ slave: number; duration: number }>();
  const params = { duration } as ProcedureRequest;

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

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
