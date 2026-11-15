// Read bytes from an ESC register of a device (GET .../registers/{address}?length=N).
//
// The program prints the bytes and, for a read of up to 8 bytes, their little-endian unsigned value.
// The ESC (EtherCAT Slave Controller) answers in every AL state.
//
// Opens the fieldbus driver and scans first when needed, and moves no device.
//
// The defaults read the type, revision and build of the ESC, 4 bytes at 0x0000.
//
//     pnpm example examples/read-register.ts
//     pnpm example examples/read-register.ts --address 0x0130 --length 2 --slave 2

import {
  formatHex,
  integerFromBytes,
  MotionMasterClient,
  run,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, address, length } = cli()
    .description(
      'Read bytes from an ESC register of a device (GET .../registers/{address}?length=N).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .addOption(
      new Option('--address <n>', 'the address of the register')
        .argParser(integer)
        .default(0x0000, '0x0000'),
    )
    .option('--length <n>', 'number of bytes', integer, 4)
    .parse()
    .opts<{ slave: number; address: number; length: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  const { data } = (await client.api.readRegister(slave, address, { length })).data;

  const bytes = data.map((b) => b.toString(16).padStart(2, '0')).join(' ');
  let line = `slave ${slave} ${formatHex(address)}: ${bytes}`;
  if (data.length <= 8) {
    const value = integerFromBytes(data);
    line += ` = ${value} (0x${value.toString(16)})`;
  }
  console.log(line);
}

await run(main);
