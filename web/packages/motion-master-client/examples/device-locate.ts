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

import { parseArgs } from 'node:util';
import {
  AlState,
  formatStep,
  MotionMasterClient,
  ProcedureError,
  type ProcedureRequest,
  type ProcedureSnapshot,
  stepChanges,
} from '@synapticon/motion-master-client';

const PROCEDURE = 'device-locate';

const { values: args } = parseArgs({
  options: {
    slave: { type: 'string', default: '1' },
    duration: { type: 'string', default: '10' },
  },
});
const slave = Number(args.slave);
// The duration in seconds, 1 to 3600, as the procedure defines it.
const params = { duration: Number(args.duration) } as ProcedureRequest;

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
  if (e instanceof ProcedureError) {
    console.error(e.message);
    process.exit(1);
  }
  if (controller.signal.aborted) {
    console.error('\ncancelled');
    process.exit(1);
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
