// Read a file from a device over FoE (GET /api/devices/{slavePosition}/files/{filename}).
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP. The server returns the
// raw bytes of the file, and the program writes them to disk.
//
// The file goes into the git-ignored files/ directory next to the client, so a download never
// shows up as untracked in the repository. Pass --output for a different path.
//
//     pnpm example examples/read-file.ts --filename .hardware_description
//     pnpm example examples/read-file.ts --slave 2 --filename config.csv --output /tmp/config.csv

import { mkdirSync, writeFileSync } from 'node:fs';
import { basename, dirname } from 'node:path';
import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { FILES_DIR } from './files';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave, filename, output } = cli()
    .description(
      'Read a file from a device over FoE (GET /api/devices/{slavePosition}/files/{filename}).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--filename <name>', 'FoE filename as the firmware knows it')
    .option('--output <path>', 'local path to save to (default: files/<filename>)')
    .parse()
    .opts<{ slave: number; filename: string; output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  // No response format: the generated client returns the answer with its body unread.
  const response = await client.api.foeReadFile(slave, filename);
  const content = new Uint8Array(await response.arrayBuffer());

  const out = output ?? `${FILES_DIR}/${basename(filename)}`;
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, content);
  console.log(`read ${content.length} byte(s) from slave ${slave}:${filename} -> ${out}`);
}

await run(main);
