// Print the PDO mapping of a device, grouped by PDO (GET .../pdo-mapping).
//
// The server reads the mapping from the object dictionary of the device: 0x1C12 and 0x1C13 name the
// assigned PDOs, and each PDO object lists the objects it maps. The program prints each entry with
// its bit offset from the start of the outputs or the inputs of the device, its bit length and its
// name. --output also saves the mapping as JSON, in the shape that write-pdo-mapping.ts takes.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/read-pdo-mapping.ts
//     pnpm example examples/read-pdo-mapping.ts --slave 2
//     pnpm example examples/read-pdo-mapping.ts --output files/pdo-mapping.json

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { AlState, formatHex, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, output } = cli()
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--output <path>', 'also save the mapping as JSON to this path')
    .parse()
    .opts<{ slave: number; output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const mapping = (await client.api.getDevicePdoMapping(slave)).data;
  const parameters = await client.parameters(slave);
  const names = new Map(parameters.map((p) => [`${p.index}:${p.subindex}`, p.name]));

  if (output !== undefined) {
    mkdirSync(dirname(output), { recursive: true });
    writeFileSync(output, JSON.stringify(mapping, null, 2));
    console.log(`saved -> ${output}`);
  }

  for (const direction of ['outputs', 'inputs'] as const) {
    console.log(`${direction}:`);
    for (const pdo of mapping[direction]) {
      console.log(`  ${formatHex(pdo.pdoIndex)}`);
      for (const e of pdo.entries) {
        const name = names.get(`${e.index}:${e.subindex}`) ?? '';
        console.log(
          `    bit ${String(e.bitOffset).padStart(4)}  ${String(e.bitLength).padStart(2)} bit(s)  ` +
            `${formatHex(e.index)}:${formatHex(e.subindex, 2, false)}  ${name}`,
        );
      }
    }
  }
}

await run(main);
