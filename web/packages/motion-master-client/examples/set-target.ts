// Set the position, velocity or torque target of a CiA 402 drive (POST .../cia402/target).
//
// The value is in the units of the object: 0x607A for position, 0x60FF for velocity, and 0x6071 for
// torque, in per mille of rated torque. Choose the target that matches the operation mode of the
// drive. A drive that is enabled in that mode moves to the target at once. The server writes the
// target in the process data when the device exchanges it, and with an SDO write otherwise.
// cia402-state.ts prints the targets and the actual values.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/set-target.ts --target velocity --value 1000
//     pnpm example examples/set-target.ts --target position --value 0 --slave 2

import { parseArgs } from 'node:util';
import {
  AlState,
  type Cia402TargetKind,
  MotionMasterClient,
} from '@synapticon/motion-master-client';

const KINDS: Cia402TargetKind[] = ['position', 'velocity', 'torque'];

const { values: args } = parseArgs({
  options: {
    slave: { type: 'string', default: '1' },
    target: { type: 'string' },
    value: { type: 'string' },
  },
});
const kind = KINDS.find((k) => k === args.target);
if (kind === undefined || args.value === undefined) {
  throw new Error(`--target must be one of ${KINDS.join(', ')}, and --value is required`);
}
const slave = Number(args.slave);
const value = Number(args.value);

const client = await MotionMasterClient.fromConfig();
await client.init();
await client.ensureState(AlState.PREOP, { positions: [slave] });

await client.setTarget(slave, kind, value);
console.log(`slave ${slave} ${kind} target = ${value}`);
