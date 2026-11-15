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

import {
  AlState,
  MotionMasterClient,
  objectAddressText,
  run,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, index, subindex, cache } = cli()
    .description(
      'Read one parameter with its name, data type and value (GET .../parameters/{index}/{subindex}).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .addOption(
      new Option('--index <n>', 'the index of the object')
        .argParser(integer)
        .default(0x6041, '0x6041'),
    )
    .option('--subindex <n>', 'the subindex of the object', integer, 0)
    .option('--cache', 'return the cached value, read no bus')
    .parse()
    .opts<{ slave: number; index: number; subindex: number; cache?: boolean }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const query = cache ? { source: 'cache' as const } : undefined;
  const p = (await client.api.readParameter(slave, index, subindex, query)).data;
  console.log(
    `slave ${slave} ${objectAddressText(p.index, p.subindex)} ` +
      `${p.name} (${p.dataTypeName}) = ` +
      JSON.stringify(p.value),
  );
}

await run(main);
