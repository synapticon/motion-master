// Write a raw SII image to the EEPROM of a device (PUT /api/devices/{slavePosition}/sii).
//
// The SII (Slave Information Interface) holds the identity and the configuration of the device. The
// server parses the image first and refuses one that is not a well-formed SII. A wrong image that
// parses can still leave the device unidentifiable until it is flashed again, so write only an
// image you read from the same kind of device, with read-sii.ts --output. The device adopts the new
// SII only after a power cycle.
//
// Opens the fieldbus driver and scans first when needed, and moves no device. The write is most
// reliable in INIT or PREOP.
//
//     pnpm example examples/write-sii.ts --file files/sii.bin

import { readFileSync } from 'node:fs';
import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, file } = cli()
    .description(
      'Write a raw SII image to the EEPROM of a device (PUT /api/devices/{slavePosition}/sii).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--file <path>', 'the raw SII image, as read-sii.ts --output saves it')
    .parse()
    .opts<{ slave: number; file: string }>();
  const image = readFileSync(file);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  await client.writeSii(slave, new Uint8Array(image));
  console.log(`wrote ${image.length} byte(s) of SII to slave ${slave}. Power cycle it to apply.`);
}

await run(main);
