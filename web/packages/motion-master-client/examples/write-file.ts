// Write a local file to a device over FoE (PUT /api/devices/{slavePosition}/files/{filename}).
//
// The file keeps its local name on the device unless --filename gives another one. A file of that
// name on the device is replaced. On a SOMANET drive, config.csv holds the stored parameter values,
// so a write to it changes the drive after the next power cycle. An Integro accepts a write only to
// a file name on its whitelist. It answers another name with FoE error 0x8001, "Path not found".
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP.
//
//     pnpm example examples/write-file.ts --file notes.txt
//     pnpm example examples/write-file.ts --file files/config.csv --filename config.csv --slave 2

import { readFileSync } from 'node:fs';
import { basename } from 'node:path';
import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const {
    slave,
    file,
    filename: name,
  } = cli()
    .description(
      'Write a local file to a device over FoE (PUT /api/devices/{slavePosition}/files/{filename}).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--file <path>', 'the local file to write')
    .option('--filename <name>', 'the name on the device (default: the local name)')
    .parse()
    .opts<{ slave: number; file: string; filename?: string }>();
  const local = file;
  const filename = name || basename(local);
  const content = readFileSync(local);

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  await client.writeFile(slave, filename, content);
  console.log(`wrote ${content.length} byte(s) from ${local} -> slave ${slave}:${filename}`);
}

await run(main);
