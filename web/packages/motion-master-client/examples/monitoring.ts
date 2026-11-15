// Record a velocity move with a monitoring, print it, and save every cycle as CSV.
//
// The motor of the drive turns. Make sure it can turn freely before you run this.
//
// The program brings the drive to OP and starts a monitoring of seven objects: Timestamp (0x20F0),
// Statusword (0x6041), Modes of operation display (0x6061), Target velocity (0x60FF), Velocity
// actual value (0x606C), Position actual value (0x6064) and Torque actual value (0x6077). While the
// monitoring records, it selects Cyclic Synchronous Velocity (CSV, mode 9), enables the drive, sets
// the target velocity, holds it for --seconds, sets it back to 0, and disables the drive.
//
// In CSV the drive takes the target velocity of every cycle as it is, with no profile ramp of its
// own, so the target steps from 0 to --velocity and back. The velocity is in the units of 0x60FF.
//
// client.monitoring() records every cycle while its span runs, also the cycles between two flushes:
// an object in the process data gives one value per cycle. After the move, the recording shows its
// rows: m.printTable() prints one row every --every ms, with the CiA402 state beside the statusword,
// and m.writeCsv() writes every row to a CSV file. Both name a column after the device position and
// the object, such as 1:VELOCITY_ACTUAL_VALUE. The program also prints how fast the velocity reached
// the target and how close it held it, from m.first() and m.values().
//
// Two clocks are in each row. The row timestamp is the time on the server when the cycle ran.
// Timestamp (0x20F0) is the clock of the drive, which it sends in the process data. It wraps, so a
// drop in it is not a restart of the drive. Use the row timestamp for the time of a cycle.
//
//     pnpm example examples/monitoring.ts
//     pnpm example examples/monitoring.ts --slave 2 --velocity 500 --seconds 3 --output /tmp/move.csv

import { setTimeout as sleep } from 'node:timers/promises';
import {
  AlState,
  Cia402,
  type MonitoringRecording,
  MotionMasterClient,
  OperationMode,
  run,
  Somanet,
} from '@synapticon/motion-master-client';
import { FILES_DIR, fileStamp } from './files';
import { cli, float, integer } from './options';

// The monitored objects, in the order of the columns.
const OBJECTS = [
  Somanet.TIMESTAMP,
  Cia402.STATUSWORD,
  Cia402.MODES_OF_OPERATION_DISPLAY,
  Cia402.TARGET_VELOCITY,
  Cia402.VELOCITY_ACTUAL_VALUE,
  Cia402.POSITION_ACTUAL_VALUE,
  Cia402.TORQUE_ACTUAL_VALUE,
];

// How long the recording goes on after the target is back at 0, so that it holds the stop.
const AFTER_STOP_MS = 1000;

/** Prints how long the actual velocity took to reach the target, and how close it held it. */
function printSummary(m: MonitoringRecording, slave: number, velocity: number): void {
  const tTarget = m.first(slave, Cia402.TARGET_VELOCITY, (v) => v === velocity);
  if (tTarget === null) {
    console.log(`the target velocity never read ${velocity}`);
    return;
  }
  const tOff = m.first(slave, Cia402.TARGET_VELOCITY, (v) => v !== velocity, { after: tTarget });
  const held = m
    .values(slave, Cia402.VELOCITY_ACTUAL_VALUE, { start: tTarget, end: tOff })
    .filter((v): v is number => v !== null);
  // The second half of the hold, where the step response has settled.
  const settled = held.slice(Math.floor(held.length / 2));
  if (settled.length === 0) {
    console.log('no velocity was recorded while the target was held');
    return;
  }
  const mean = settled.reduce((a, b) => a + b, 0) / settled.length;
  // A spread of a long hold into Math.min would pass too many arguments, so reduce instead.
  const min = settled.reduce((a, b) => Math.min(a, b));
  const max = settled.reduce((a, b) => Math.max(a, b));
  // Measured in the direction of the target, so that a negative velocity works too.
  const direction = Math.sign(velocity);
  const tReached = m.first(
    slave,
    Cia402.VELOCITY_ACTUAL_VALUE,
    (v) => v !== null && v * direction >= Math.abs(velocity) * 0.95,
    { after: tTarget },
  );
  const reached = tReached !== null ? `${((tReached - tTarget) / 1000).toFixed(0)} ms` : 'never';
  const first = m.rows[0][0] as number;
  const last = m.rows[m.rows.length - 1][0] as number;
  console.log(
    `${m.rows.length} cycles over ${((last - first) / 1e6).toFixed(2)} s; ` +
      `95 % of the target after ${reached}; second half of the hold: mean ${mean.toFixed(1)}, ` +
      `min ${min}, max ${max} over ${settled.length} cycles`,
  );
}

async function main(): Promise<void> {
  const { slave, velocity, seconds, every, output } = cli()
    .description('Record a velocity move with a monitoring, print it, and save every cycle as CSV.')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--velocity <n>', 'target velocity, units of 0x60FF', integer, 250)
    .option('--seconds <s>', 'how long to hold the velocity', float, 2)
    .option('--every <ms>', 'print one line every N ms', integer, 100)
    .option('--output <path>', 'CSV file (default: files/monitoring-<time>.csv)')
    .parse()
    .opts<{ slave: number; velocity: number; seconds: number; every: number; output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.OP, { positions: [slave] });

  // Ctrl-C aborts the signal. Without it, Node.js ends at once and skips the finally block below,
  // which leaves the drive enabled and turning.
  const controller = new AbortController();
  const { signal } = controller;
  process.once('SIGINT', () => controller.abort(new Error('cancelled')));

  const parameters = OBJECTS.map((address) => [slave, address] as const);
  let m: MonitoringRecording;
  try {
    m = await client.monitoring(
      parameters,
      async () => {
        await client.setOperationMode(slave, OperationMode.CYCLIC_SYNC_VELOCITY);
        // A target left from an earlier run would turn the motor at the enable.
        await client.setTarget(slave, 'velocity', 0);
        await client.enable(slave);
        console.log(`turning at ${velocity} for ${seconds} s`);
        await client.setTarget(slave, 'velocity', velocity);
        await sleep(seconds * 1000, undefined, { signal });
        await client.setTarget(slave, 'velocity', 0);
        await sleep(AFTER_STOP_MS, undefined, { signal });
      },
      { intervalMs: 50, signal },
    );
  } catch (error) {
    if (signal.aborted) {
      console.error('cancelled');
      process.exitCode = 1;
      return;
    }
    throw error;
  } finally {
    // Never leave the drive enabled, also when a step above failed or Ctrl-C stopped it.
    await client.disable(slave).catch(() => {});
  }

  m.names.forEach((name, i) => {
    if (m.sources[i] !== 'pdo') {
      console.log(`note: ${name} is not in the process data, so it has one value per flush`);
    }
  });

  m.printTable({ everyMs: every });
  printSummary(m, slave, velocity);

  const out = output ?? `${FILES_DIR}/monitoring-${fileStamp()}.csv`;
  await m.writeCsv(out);
  console.log(`saved ${m.rows.length} row(s) -> ${out}`);
}

await run(main);
