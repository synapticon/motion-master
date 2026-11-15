// Write the PDO mapping of a device from a JSON file (PUT .../pdo-mapping).
//
// The file has the shape that read-pdo-mapping.ts --output saves: "outputs" lists the RxPDO objects
// (0x16xx) and "inputs" the TxPDO objects (0x1Axx), each with its entries in order. The order of the
// objects is the order of the sync manager assignment. An empty list clears that direction. The
// program ignores the bitOffset of an entry, because the device derives the offsets.
//
// The mapping objects are writable only in PREOP, so the program restarts the device through INIT to
// PREOP, writes the mapping, and moves the device back to SAFEOP or OP when it was there. The server
// then maps the process image of the whole bus again from the new mapping, which pauses the bus
// briefly. The program prints the mapping that the device reports back.
//
// Opens the fieldbus driver and scans first when needed.
//
//     pnpm example examples/read-pdo-mapping.ts --output files/pdo-mapping.json
//     pnpm example examples/write-pdo-mapping.ts --file files/pdo-mapping.json

import { readFileSync } from 'node:fs';
import {
  AlState,
  formatHex,
  HttpError,
  MotionMasterClient,
  type PdoMapping,
  type PdoMappingRequestObject,
  objectAddressText,
  run,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';

/** Returns the PDO objects of one direction without the bit offsets that the device derives. */
function requestObjects(objects: PdoMapping['outputs']): PdoMappingRequestObject[] {
  return objects.map((o) => ({
    pdoIndex: o.pdoIndex,
    entries: o.entries.map((e) => ({
      index: e.index,
      subindex: e.subindex,
      bitLength: e.bitLength,
    })),
  }));
}

async function main(): Promise<void> {
  const { slave, file } = cli()
    .description('Write the PDO mapping of a device from a JSON file (PUT .../pdo-mapping).')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--file <path>', 'the mapping as JSON')
    .parse()
    .opts<{ slave: number; file: string }>();
  const mapping = JSON.parse(readFileSync(file, 'utf8')) as PdoMapping;
  const body = {
    outputs: requestObjects(mapping.outputs),
    inputs: requestObjects(mapping.inputs),
  };

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  const query = { positions: String(slave) };
  const previous = (await client.api.getDeviceStates(query)).data[0].alState;

  // Through INIT, never down from SAFEOP or OP. On an Integro, a write of the mapping objects aborts
  // with 0x08000000 after SAFEOP -> PREOP or OP -> PREOP, and passes after INIT -> PREOP. The
  // firmware source points at its application processor, which learns only of a transition upwards
  // and so checks the write against the rights of the state before.
  const restart = (await client.api.restartDevices({ positions: [slave], target: AlState.PREOP }))
    .data;
  if (!restart.ok) {
    console.error(`slave ${slave} did not reach PREOP through INIT`);
    process.exitCode = 1;
    return;
  }

  let readBack: PdoMapping;
  try {
    readBack = (await client.api.writeDevicePdoMapping(slave, body)).data;
  } catch (e) {
    // A failed write is not undone, so the device stays in PREOP, where its mapping can be read and
    // written again.
    if (e instanceof HttpError) {
      console.error(`error: ${e.message}\nslave ${slave} stays in PREOP`);
      process.exitCode = 1;
      return;
    }
    throw e;
  }

  // A device that came from below PREOP stays in PREOP, which is where its mapping applies.
  if (previous === AlState.SAFEOP || previous === AlState.OP) {
    await client.ensureState(previous, { positions: [slave], exact: true });
  }

  for (const direction of ['outputs', 'inputs'] as const) {
    console.log(`${direction}:`);
    for (const pdo of readBack[direction]) {
      const entries = pdo.entries
        .map((e) => `${objectAddressText(e.index, e.subindex)}/${e.bitLength}`)
        .join(', ');
      console.log(`  ${formatHex(pdo.pdoIndex)}  ${entries}`);
    }
  }
}

await run(main);
