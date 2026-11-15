// Print the CiA 402 state of a drive with its actual and target values (GET .../cia402).
//
// CiA 402 is the device profile for drives. The server decodes the Statusword into the state of the
// profile's state machine, and reads the operation mode, the actual values, the targets and the
// drive's error report.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/cia402-state.ts
//     pnpm example examples/cia402-state.ts --slave 2

import { parseArgs } from 'node:util';
import { AlState, MotionMasterClient } from '@synapticon/motion-master-client';

const { values: args } = parseArgs({ options: { slave: { type: 'string', default: '1' } } });
const slave = Number(args.slave);

const client = await MotionMasterClient.fromConfig();
await client.init();
await client.ensureState(AlState.PREOP, { positions: [slave] });

const s = (await client.api.getCia402Status(slave)).data;
const hex = (value: number) => `0x${value.toString(16).padStart(4, '0')}`;
const column = (value: number | null) => String(value).padStart(12);
console.log(`state ${s.state}, statusword ${hex(s.statusword)}, controlword ${hex(s.controlword)}`);
console.log(`mode ${s.modeName} (${s.operationMode}), error report ${s.errorDescription}`);
console.log(`  position  actual ${column(s.actualPosition)}  target ${column(s.targetPosition)}`);
console.log(`  velocity  actual ${column(s.actualVelocity)}  target ${column(s.targetVelocity)}`);
console.log(`  torque    actual ${column(s.actualTorque)}  target ${column(s.targetTorque)}`);
