// Move the devices to an EtherCAT AL state.
//
// Opens the fieldbus driver and scans first when needed. The server walks each device through every
// state between, one legal transition at a time (POST /api/devices/state/transition). A device that
// does not reach the state stops the program with its AL status code.
//
//     pnpm example examples/transition-state.ts --state op
//     pnpm example examples/transition-state.ts --state preop --slave 2

import {
  AlState,
  alStateName,
  MotionMasterClient,
  run,
  type TargetAlState,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

// The states a device can be commanded to, by their names in lowercase: init, preop, boot, ...
const STATES = Object.fromEntries(
  Object.entries(AlState)
    .filter(([name]) => name !== 'UNKNOWN')
    .map(([name, value]) => [name.toLowerCase(), value]),
) as Record<string, TargetAlState>;

async function main(): Promise<void> {
  const { state: name, slave } = cli()
    .addOption(
      new Option('--state <state>', 'the AL state to move the devices to')
        .choices(Object.keys(STATES))
        .makeOptionMandatory(),
    )
    .option('--slave <n>', 'one device position (default: every device)', integer)
    .parse()
    .opts<{ state: string; slave?: number }>();
  const state = STATES[name];
  const positions = slave ? [slave] : undefined;

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(state, { positions, exact: true });

  const query = slave ? { positions: String(slave) } : undefined;
  for (const d of (await client.api.getDeviceStates(query)).data) {
    console.log(`  ${String(d.slavePosition).padStart(3)}  ${alStateName(d.alState)}`);
  }
}

await run(main);
