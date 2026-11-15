// Print what a device says it is (GET /api/devices/{slavePosition}/hardware-description).
//
// The server reads the .hardware_description file of the device over FoE and decodes it. The file
// names the device and, when the device is part of one, the assembly, with the build descriptor, the
// serial number and the components of each. A device that is not a SOMANET device has no such file,
// and the request fails.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP.
//
//     pnpm example examples/read-hardware-description.ts
//     pnpm example examples/read-hardware-description.ts --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

type HardwareDescription = Awaited<ReturnType<MotionMasterClient['hardwareDescription']>>;

/** Prints one part of a hardware description: the device or the assembly. */
function printPart(title: string, part: HardwareDescription['device']): void {
  console.log(`${title}: ${part.name}`);
  console.log(`  build descriptor ${part.buildDescriptor}, serial number ${part.serialNumber}`);
  for (const c of part.components ?? []) {
    const version = c.version ? `, version ${c.version}` : '';
    console.log(`  - ${c.name}, serial number ${c.serialNumber}${version}`);
  }
}

async function main(): Promise<void> {
  const { slave } = cli()
    .description(
      'Print what a device says it is (GET /api/devices/{slavePosition}/hardware-description).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const hd = await client.hardwareDescription(slave);
  printPart('device', hd.device);
  if (hd.assembly) {
    printPart('assembly', hd.assembly);
  }
}

await run(main);
