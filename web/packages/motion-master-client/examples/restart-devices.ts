// Restart devices through INIT up to an AL state (POST /api/devices/state/restart).
//
// The server commands each device to INIT, and then up one state at a time to --state: PREOP,
// SAFEOP, OP. A --state of BOOT goes from INIT to BOOT. The walk stops at the first state that a
// device does not reach. A restart drops every device it restarts out of the process data exchange,
// so a drive that was enabled is disabled.
//
// Opens the fieldbus driver and scans first when needed. The program exits with 1 when a device does
// not reach --state.
//
//     pnpm example examples/restart-devices.ts
//     pnpm example examples/restart-devices.ts --state preop --slave 2

import { parseArgs } from 'node:util';
import {
  AlState,
  alStateName,
  formatHex,
  MotionMasterClient,
  run,
  type TargetAlState,
} from '@synapticon/motion-master-client';

// The states a device can be commanded to, by their names in lowercase: init, preop, boot, ...
const STATES = Object.fromEntries(
  Object.entries(AlState)
    .filter(([name]) => name !== 'UNKNOWN')
    .map(([name, value]) => [name.toLowerCase(), value]),
) as Record<string, TargetAlState>;

async function main(): Promise<void> {
  const { values: args } = parseArgs({
    options: {
      state: { type: 'string', default: 'op' },
      slave: { type: 'string' },
    },
  });
  const target = STATES[args.state];
  if (target === undefined) {
    throw new Error(`--state must be one of ${Object.keys(STATES).join(', ')}`);
  }

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  const body = args.slave ? { target, positions: [Number(args.slave)] } : { target };
  const result = (await client.api.restartDevices(body)).data;
  for (const d of result.devices) {
    const mark = d.reached ? '' : `  not reached, alStatusCode ${formatHex(d.alStatusCode)}`;
    console.log(`  ${String(d.slavePosition).padStart(3)}  ${alStateName(d.alState)}${mark}`);
  }
  if (!result.ok) {
    console.error(`the restart stopped at ${alStateName(result.step)}`);
    process.exitCode = 1;
  }
}

await run(main);
