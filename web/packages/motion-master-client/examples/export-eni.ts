// Save the bus as an ENI file (GET /api/eni).
//
// An ENI (EtherCAT Network Information, ETG.2100) is the configuration that another EtherCAT master
// replays to bring the same bus up. The server builds it from the live bus. The file goes into the
// git-ignored files/ directory next to the client. Pass --output for a different path.
//
// Opens the fieldbus driver, scans and moves the devices to SAFEOP first when needed, because the
// FMMUs and the logical addresses of the bus exist only from the SAFEOP transition on. A device in OP
// stays there.
//
//     pnpm example examples/export-eni.ts
//     pnpm example examples/export-eni.ts --output /tmp/bus.xml

import { mkdirSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { FILES_DIR } from './files';
import { cli } from './options';

async function main(): Promise<void> {
  const { output } = cli()
    .option('--output <path>', 'local path to save to (default: files/eni.xml)')
    .parse()
    .opts<{ output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.SAFEOP);

  const eni = await client.exportEni();
  const out = output ?? `${FILES_DIR}/eni.xml`;
  mkdirSync(dirname(out), { recursive: true });
  writeFileSync(out, eni.xml, 'utf8');
  console.log(`saved ${eni.xml.length} character(s) -> ${out}`);
  if (eni.warnings) {
    console.log(`${eni.warnings} part(s) of the bus could not be read; the server log says which`);
  }
}

await run(main);
