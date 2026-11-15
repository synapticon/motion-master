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
  formatHexBytes,
  MotionMasterClient,
  run,
} from '@synapticon/motion-master-client';
import { byteList, cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, address, data } = cli()
    .description('Write bytes to an ESC register of a device (POST .../registers/{address}).')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--address <n>', 'the address of the register', integer)
    .requiredOption('--data <bytes>', 'bytes separated by commas, such as 0,0', byteList)
    .parse()
    .opts<{ slave: number; address: number; data: number[] }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  await client.api.writeRegister(slave, address, { data });
  console.log(`slave ${slave} ${formatHex(address)} <- ${formatHexBytes(data)}`);
}

await run(main);
