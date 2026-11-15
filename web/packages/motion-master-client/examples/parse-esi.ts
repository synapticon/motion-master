// Parse an ESI file and list the devices it describes (POST /api/esi/parse).
//
// An ESI (EtherCAT Slave Information) is the XML file in which a vendor describes its devices. The
// server parses it offline and returns every device with its own object table, one entry per index
// and subindex, with the objects of its modules merged in. The program prints a line per device.
// --output saves the whole response as JSON, entries and warnings included.
//
// The program needs no bus and no fieldbus driver, so it opens none and leaves the server as it is.
//
//     pnpm example examples/parse-esi.ts ../../../libs/etg/tests/data/somanet-v5.6.6.xml
//     pnpm example examples/parse-esi.ts somanet.xml --output files/esi.json

import { mkdirSync, readFileSync, writeFileSync } from 'node:fs';
import { dirname } from 'node:path';
import {
  type EsiParseResult,
  formatHex,
  HttpError,
  MotionMasterClient,
  run,
} from '@synapticon/motion-master-client';
import { cli } from './options';

async function main(): Promise<void> {
  const command = cli()
    .description('Parse an ESI file and list the devices it describes (POST /api/esi/parse).')
    .argument('<esi>', 'path to the ESI XML file')
    .option('--output <path>', 'save the whole parsed response as JSON to this path')
    .parse();
  const [esi] = command.args;
  const args = command.opts<{ output?: string }>();

  const client = await MotionMasterClient.fromConfig();
  // The generated api.parseEsi sends a body it has no formatter for as JSON, so the document would
  // arrive as a quoted JSON string.
  const url = `${client.api.baseUrl}/api/esi/parse`;
  const response = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/xml' },
    body: readFileSync(esi),
  });
  if (!response.ok) {
    throw await HttpError.from(new Request(url, { method: 'POST' }), response);
  }
  const result = (await response.json()) as EsiParseResult;

  if (args.output !== undefined) {
    const out = args.output;
    mkdirSync(dirname(out), { recursive: true });
    writeFileSync(out, JSON.stringify(result, null, 2));
    console.log(`saved -> ${out}`);
  }

  const vendor = result.vendor;
  console.log(
    `${vendor.name} (vendor ${formatHex(vendor.id, 8)}), ESI version ${String(result.version)}`,
  );
  for (const d of result.devices) {
    console.log(
      `  ${d.name}: product ${formatHex(d.productCode ?? 0, 8)}, ` +
        `revision ${formatHex(d.revisionNo ?? 0, 8)}, ` +
        `${d.objectCount} object(s) of its own, ${(d.entries ?? []).length} entries with its ` +
        `modules, ${d.rxPdoCount ?? 0} RxPDO(s), ${d.txPdoCount ?? 0} TxPDO(s), ` +
        `${(d.warnings ?? []).length} warning(s)`,
    );
  }
}

await run(main);
