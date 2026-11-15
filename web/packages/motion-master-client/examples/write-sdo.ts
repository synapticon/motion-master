// Write raw bytes to one object with an SDO download (PUT .../sdo/{index}/{subindex}).
//
// --data gives the bytes in wire order, little-endian, separated by commas. The byte count must
// match the length of the object. write-parameter.ts converts a value to the data type instead,
// which is the easier way for most objects.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
// mailbox is live from PREOP up. A device in SAFEOP or OP stays there.
//
//     pnpm example examples/write-sdo.ts --index 0x6060 --subindex 0 --data 8
//     pnpm example examples/write-sdo.ts --index 0x607a --subindex 0 --data 0x10,0x27,0,0 --slave 2

import {
  AlState,
  formatHexBytes,
  MotionMasterClient,
  objectAddressText,
  run,
} from '@synapticon/motion-master-client';
import { byteList, cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, index, subindex, data } = cli()
    .description(
      'Write raw bytes to one object with an SDO download (PUT .../sdo/{index}/{subindex}).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--index <n>', 'the index of the object', integer)
    .requiredOption('--subindex <n>', 'the subindex of the object', integer)
    .requiredOption('--data <bytes>', 'bytes separated by commas, such as 0x10,0x27', byteList)
    .parse()
    .opts<{ slave: number; index: number; subindex: number; data: number[] }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  await client.writeSdo(slave, index, subindex, data);
  console.log(`slave ${slave} ${objectAddressText(index, subindex)} <- ${formatHexBytes(data)}`);
}

await run(main);
