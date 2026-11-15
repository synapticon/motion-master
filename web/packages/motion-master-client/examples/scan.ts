// Scan the bus and list the devices it finds (POST /api/scan).
//
// Opens the fieldbus driver from config.jsonc first when none is open. A scan leaves every device in
// INIT. An empty bus is a successful scan that finds no devices, not an error.
//
//     pnpm example examples/scan.ts

import { alStateName, MotionMasterClient, run } from '@synapticon/motion-master-client';

async function main(): Promise<void> {
  const client = await MotionMasterClient.fromConfig();
  await client.init();

  const found = await client.scan();
  if (found > 0) {
    const states = new Map(
      (await client.api.getDeviceStates()).data.map((d) => [d.slavePosition, d]),
    );
    for (const d of await client.devices()) {
      const state = alStateName(states.get(d.slavePosition)?.alState ?? 0);
      console.log(
        `  ${String(d.slavePosition).padStart(3)}  ${d.productName.padEnd(30)}  ` +
          `serial ${String(d.serialNumber).padEnd(12)}  ` +
          `revision 0x${d.revisionNumber.toString(16).padStart(8, '0')}  ${state}`,
      );
    }
  }
}

await run(main);
