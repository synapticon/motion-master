// List the files stored on a device with their sizes (GET /api/devices/{slavePosition}/files).
//
// The size is what the file list of the device reports. A file name the device reports with no size
// is shown without one.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there. A device in BOOT goes through INIT to PREOP.
//
//     pnpm example examples/list-files.ts
//     pnpm example examples/list-files.ts --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { slave } = cli()
    .description(
      'List the files stored on a device with their sizes (GET /api/devices/{slavePosition}/files).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const files = await client.listFiles(slave);
  for (const f of files) {
    const size =
      f.byteCount === undefined ? ' '.repeat(14) : `${String(f.byteCount).padStart(8)} bytes`;
    console.log(`  ${size}  ${f.name}`);
  }
  console.log(`${files.length} file(s)`);
}

await run(main);
