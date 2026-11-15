// Configure the system identification chirp of a drive, and arm it (system-identification).
//
// The chirp is a torque sweep from a start frequency to a target frequency over a time you give:
// logarithmic, with an amplitude that rises from half the target, or linear, at a constant
// amplitude. With --start none, the default, the procedure configures the drive and excites
// nothing. With --start immediately, the chirp starts on the next control cycle if the drive is
// enabled, so the shaft must be free. With --start after-hrd-stream-start, it waits for a high
// resolution data recording to begin, which is how the response gets recorded.
//
// The amplitude is a torque command, and neither the server nor the drive checks it. The server
// checks the frequency and time bounds before it writes anything. Ctrl-C cancels the run.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/system-identification.ts --start-frequency 1000 --target-frequency 100000 \
//         --amplitude 50 --time 5000

import {
  AlState,
  formatStep,
  MotionMasterClient,
  type ProcedureRequest,
  type ProcedureSnapshot,
  run,
  stepChanges,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

const PROCEDURE = 'system-identification';
const SIGNALS = ['logarithmic', 'linear'];
const STARTS = ['none', 'immediately', 'after-hrd-stream-start'];

async function main(): Promise<void> {
  const { slave, startFrequency, targetFrequency, amplitude, time, signal, start } = cli()
    .description(
      'Configure the system identification chirp of a drive, and arm it (system-identification).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--start-frequency <n>', 'sweep start, mHz', integer)
    .requiredOption('--target-frequency <n>', 'sweep end, mHz', integer)
    .requiredOption('--amplitude <n>', 'peak torque, per mille of rated torque', integer)
    .requiredOption('--time <n>', 'sweep duration, ms', integer)
    .addOption(
      new Option('--signal <type>', 'the shape of the sweep')
        .choices(SIGNALS)
        .default('logarithmic'),
    )
    .addOption(
      new Option('--start <when>', 'when the chirp starts').choices(STARTS).default('none'),
    )
    .parse()
    .opts<{
      slave: number;
      startFrequency: number;
      targetFrequency: number;
      amplitude: number;
      time: number;
      signal: string;
      start: string;
    }>();
  // The swagger.yml schema of a procedure request does not list the chirp parameters.
  const params = {
    startFrequencyMilliHz: startFrequency,
    targetFrequencyMilliHz: targetFrequency,
    targetAmplitudePermil: amplitude,
    transitionTimeMs: time,
    signalType: signal,
    start,
  } as ProcedureRequest;

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
