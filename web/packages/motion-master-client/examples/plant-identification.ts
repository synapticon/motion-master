// Identify the plant model of a drive, and write plant_model.csv (plant-identification).
//
// The plant model is the transfer function from torque to velocity of the motor and its load, from
// which auto-tuning computes controller gains. The procedure enables the drive in cyclic synchronous
// torque mode with a zero target, runs a chirp, records the velocity and the torque, and fits the
// model with the auto-tuning program of the server. The chirp turns the motor in both directions
// for its whole duration, so the shaft must be free and its load safe to move. The drive records at
// most 6000 ms, so --time can be at most 5800 ms. The amplitude is a torque command, and neither the
// server nor the drive checks it. The controller gains do not change. Ctrl-C cancels the run.
//
// Opens the fieldbus driver, scans and moves the device to OP first when needed, because the drive
// enables only while it exchanges process data.
//
//     pnpm example examples/plant-identification.ts --start-frequency 1000 --target-frequency 100000 \
//         --amplitude 50 --time 5000

import {
  AlState,
  MotionMasterClient,
  type ProcedureRequest,
  run,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';
import { runToEnd } from './procedures';

const PROCEDURE = 'plant-identification';
const SIGNALS = ['logarithmic', 'linear'];

async function main(): Promise<void> {
  const { slave, startFrequency, targetFrequency, amplitude, time, signal } = cli()
    .description(
      'Identify the plant model of a drive, and write plant_model.csv (plant-identification).',
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
    .parse()
    .opts<{
      slave: number;
      startFrequency: number;
      targetFrequency: number;
      amplitude: number;
      time: number;
      signal: NonNullable<ProcedureRequest['signalType']>;
    }>();
  const params: ProcedureRequest = {
    startFrequencyMilliHz: startFrequency,
    targetFrequencyMilliHz: targetFrequency,
    targetAmplitudePermil: amplitude,
    transitionTimeMs: time,
    signalType: signal,
  };

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.OP, { positions: [slave] });

  await runToEnd(client, slave, PROCEDURE, params);
}

await run(main);
