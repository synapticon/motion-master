// Save the recorder of the server as a .mmpd file (GET /api/process-data/dump).
//
// The server records the process data of every cycle in a ring. The dump holds every cycle that the
// ring still has, with the layout of the process image in its header, so that a reader can decode it
// offline. The Console and parseMmpd() of this client read .mmpd files. The file goes into the
// git-ignored files/ directory next to the client. Pass --output for a different path.
//
// Opens the fieldbus driver, scans and moves the device to SAFEOP first when needed, because the
// recorder holds cycles only once the process data is exchanged. A device in OP stays there.
//
//     pnpm example examples/dump-process-data.ts
//     pnpm example examples/dump-process-data.ts --output /tmp/bus.mmpd

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { FILES_DIR, fileStamp } from './files';
import { cli } from './options';

async function main(): Promise<void> {
  const { output } = cli()
    .description('Save the recorder of the server as a .mmpd file (GET /api/process-data/dump).')
    .option('--output <path>', 'local path to save to (default: files/process-data-<time>.mmpd)')
    .parse()
    .opts<{ output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.SAFEOP);

  const content = await client.processDataDump();
  if (content === null) {
    console.error('the server has no cycle to dump');
    process.exit(1);
  }

  const out = output || `${FILES_DIR}/process-data-${fileStamp()}.mmpd`;
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, content);
  console.log(`saved ${content.length} byte(s) -> ${out}`);
}

await run(main);
