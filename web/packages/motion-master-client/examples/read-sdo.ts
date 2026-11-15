// Read one object dictionary entry with a CoE SDO upload (GET .../sdo/{index}/{subindex}).
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
// mailbox is live from PREOP up. A device in SAFEOP or OP stays there. The server returns the
// raw bytes of the object. The program prints them and their little-endian unsigned value.
//
// The defaults read the Statusword, 0x6041:00.
//
//     pnpm example examples/read-sdo.ts
//     pnpm example examples/read-sdo.ts --slave 2 --index 0x6064 --subindex 0

import { AlState, formatHex, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, index, subindex } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .addOption(
      new Option('--index <n>', 'the index of the object')
        .argParser(integer)
        .default(0x6041, '0x6041'),
    )
    .option('--subindex <n>', 'the subindex of the object', integer, 0)
    .parse()
    .opts<{ slave: number; index: number; subindex: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const { data } = (await client.api.sdoUpload(slave, index, subindex)).data;

  // A bigint, because an object can be longer than the 53 bits of a number.
  const value = data.reduceRight((v, b) => (v << 8n) | BigInt(b), 0n);
  console.log(
    `slave ${slave} ${formatHex(index)}:${formatHex(subindex, 2, false)} = ` +
      `${value} (0x${value.toString(16)}), ${data.length} byte(s): [${data.join(', ')}]`,
  );
}

await run(main);
