// Write bytes to an ESC register of a device (POST .../registers/{address}).
//
// --data gives the bytes in register order, separated by commas. The server writes them with one
// FPWR datagram to the ESC (EtherCAT Slave Controller). It does not check what the register does.
// read-register.ts reads the register back.
//
// Opens the fieldbus driver and scans first when needed, and moves no device.
//
// The example clears the invalid frame and RX error counters of ports 0 to 3, 8 bytes at 0x0300,
// which only a write or a power cycle clears.
//
//     pnpm example examples/write-register.ts --address 0x0300 --data 0,0,0,0,0,0,0,0

import {
  formatHex,
  MotionMasterClient,
  parseIntegerFlexible,
  run,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';

/** Parses a decimal or a 0x-prefixed hex integer. */
function parseInteger(text: string): number {
  const value = parseIntegerFlexible(text.trim());
  if (value === null) {
    throw new Error(`'${text}' is not an integer`);
  }
  return Number(value);
}

/** Returns the bytes as lowercase hex, separated by spaces. */
function hexBytes(bytes: number[]): string {
  return bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ');
}

async function main(): Promise<void> {
  const {
    slave,
    address,
    data: text,
  } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--address <n>', 'the address of the register', integer)
    .requiredOption('--data <bytes>', 'bytes separated by commas, such as 0,0')
    .parse()
    .opts<{ slave: number; address: number; data: string }>();
  const data = text.split(',').map(parseInteger);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  await client.api.writeRegister(slave, address, { data });
  console.log(`slave ${slave} ${formatHex(address)} <- ${hexBytes(data)}`);
}

await run(main);
