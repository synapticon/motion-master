// Read the SII of a device, the configuration in its EEPROM (GET .../sii).
//
// By default the program prints the parsed SII as JSON. `info` holds the identity, the mailbox
// layout and the PDI configuration. `category` holds the strings, the general category, the FMMUs,
// the sync managers, the PDOs and the distributed clock settings. --output saves the raw EEPROM
// image instead, which write-sii.ts and POST /api/sii/parse take. The server reads the EEPROM live,
// in every AL state.
//
// Opens the fieldbus driver and scans first when needed, and moves no device.
//
//     pnpm example examples/read-sii.ts
//     pnpm example examples/read-sii.ts --slave 2 --output files/sii.bin

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, output } = cli()
    .description('Read the SII of a device, the configuration in its EEPROM (GET .../sii).')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--output <path>', 'save the raw EEPROM image to this path')
    .parse()
    .opts<{ slave: number; output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  if (output) {
    // The Accept header selects the raw image over the parsed JSON. With no response format, the
    // generated client returns the answer with its body unread.
    const response = await client.api.readSii(slave, {
      format: undefined,
      headers: { Accept: 'application/octet-stream' },
    });
    const content = new Uint8Array(await response.arrayBuffer());
    mkdirSync(dirname(output), { recursive: true });
    writeFileSync(output, content);
    console.log(`read ${content.length} byte(s) of SII from slave ${slave} -> ${output}`);
    return;
  }

  console.log(JSON.stringify((await client.api.readSii(slave)).data, null, 2));
}

await run(main);
