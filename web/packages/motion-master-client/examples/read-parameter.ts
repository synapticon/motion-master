// Read one parameter with its name, data type and value (GET .../parameters/{index}/{subindex}).
//
// The server reads the value from the process image when the object is PDO-mapped and the device
// exchanges process data, and with an SDO read otherwise. --cache returns the value the server holds
// and touches no bus.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. The server
// learns the parameters of a device when it reaches PREOP. A device in SAFEOP or OP stays there.
//
// The defaults read the Statusword, 0x6041:00.
//
//     pnpm example examples/read-parameter.ts
//     pnpm example examples/read-parameter.ts --index 0x6064 --subindex 0 --slave 2
//     pnpm example examples/read-parameter.ts --index 0x2031 --subindex 1 --cache

import { parseArgs } from 'node:util';
import { AlState, formatHex, MotionMasterClient, run } from '@synapticon/motion-master-client';

async function main(): Promise<void> {
  const { values: args } = parseArgs({
    options: {
      slave: { type: 'string', default: '1' },
      index: { type: 'string', default: '0x6041' },
      subindex: { type: 'string', default: '0' },
      cache: { type: 'boolean', default: false },
    },
  });
  // Number() reads a decimal and a 0x-prefixed hex integer.
  const slave = Number(args.slave);
  const index = Number(args.index);
  const subindex = Number(args.subindex);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const query = args.cache ? { source: 'cache' as const } : undefined;
  const p = (await client.api.readParameter(slave, index, subindex, query)).data;
  console.log(
    `slave ${slave} ${formatHex(p.index)}:${formatHex(p.subindex, 2, false)} ` +
      `${p.name} (${p.dataTypeName}) = ` +
      JSON.stringify(p.value),
  );
}

await run(main);
