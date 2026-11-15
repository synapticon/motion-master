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

import {
  AlState,
  formatStep,
  MotionMasterClient,
  type ProcedureSnapshot,
  run,
  stepChanges,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';

const PROCEDURE = 'store-parameters';

async function main(): Promise<void> {
  const { slave } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  // Ctrl-C aborts the signal, so that runProcedure() cancels the run before the program ends.
  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort(new Error('cancelled')));

  let snapshot: ProcedureSnapshot | undefined;
  try {
    for await (const [current, changed] of stepChanges(
      client.runProcedure(slave, PROCEDURE, { params: {}, signal: controller.signal }),
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
