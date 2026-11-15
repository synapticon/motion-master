// Write one parameter, converted to its data type (PUT .../parameters/{index}/{subindex}).
//
// The program reads the parameter first, and converts --value by the JSON type of its current
// value: an integer as decimal or 0x-prefixed hex, a real number as a float, a string as written,
// and a byte array as bytes separated by commas. The server then converts the value to the declared
// data type, and refuses one that the type cannot hold. It sends the value in the process data when
// the object is output-mapped and the device exchanges process data, and with an SDO write
// otherwise. The program prints the value before and after.
//
// A write to a parameter that controls the drive, such as a target, takes effect at once. Run
// store-parameters.ts to keep a value across a power cycle.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/write-parameter.ts --index 0x6060 --subindex 0 --value 8
//     pnpm example examples/write-parameter.ts --index 0x6060 --subindex 0 --value 10 --slave 2

import {
  AlState,
  MotionMasterClient,
  objectAddressText,
  type ParameterValue,
  run,
} from '@synapticon/motion-master-client';
import { byteList, cli, float, integer } from './options';

/** Converts the text of --value to the JSON type of the current value of the parameter. */
function convert(current: ParameterValue, dataTypeName: string, text: string): ParameterValue {
  if (typeof current === 'number') {
    // A JSON number does not say whether it is an integer, because 1.0 reads as 1. The data type
    // says it.
    return dataTypeName.startsWith('REAL') ? float(text) : integer(text);
  }
  if (Array.isArray(current)) {
    return byteList(text);
  }
  return text;
}

async function main(): Promise<void> {
  const {
    slave,
    index,
    subindex,
    value: text,
  } = cli()
    .description(
      'Write one parameter, converted to its data type (PUT .../parameters/{index}/{subindex}).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .requiredOption('--index <n>', 'the index of the object', integer)
    .requiredOption('--subindex <n>', 'the subindex of the object', integer)
    .requiredOption('--value <text>', 'the value, converted to the data type of the parameter')
    .parse()
    .opts<{ slave: number; index: number; subindex: number; value: string }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  const before = (await client.api.readParameter(slave, index, subindex)).data;
  let value: ParameterValue;
  try {
    value = convert(before.value, before.dataTypeName, text);
  } catch (e) {
    console.error(`--value '${text}' does not fit ${before.dataTypeName}: ${(e as Error).message}`);
    process.exit(1);
  }

  const after = await client.writeValue<ParameterValue>(slave, { index, subindex }, value);
  console.log(
    `slave ${slave} ${objectAddressText(index, subindex)} ${before.name} ` +
      `(${before.dataTypeName}): ${JSON.stringify(before.value)} -> ${JSON.stringify(after)}`,
  );
}

await run(main);
