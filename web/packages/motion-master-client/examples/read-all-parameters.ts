// Read the whole object dictionary of a device with its values and print it.
//
// POST /api/devices/{slavePosition}/parameters/init?readValues=true reads the object dictionary
// through the SDO Information service. With readValues=true it also reads and decodes the value of
// every entry in the same call. On a drive with a full dictionary this takes several seconds.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed, because the
// mailbox is live from PREOP up. A device in SAFEOP or OP stays there.
//
//     pnpm example examples/read-all-parameters.ts
//     pnpm example examples/read-all-parameters.ts --slave 2

import {
  AlState,
  type DeviceParameter,
  formatHex,
  integerFromBytes,
  MotionMasterClient,
  run,
} from '@synapticon/motion-master-client';
import { cli, integer } from './options';

/** Returns the bytes as lowercase hex, separated by spaces. */
function hexBytes(bytes: number[]): string {
  return bytes.map((b) => b.toString(16).padStart(2, '0')).join(' ');
}

/**
 * Renders the value of a DeviceParameter, and decodes a raw byte array by its data type.
 *
 * The server decodes the standard scalar and string types, which arrive as JSON numbers and
 * strings. Every other type arrives as a byte array: the odd-width integers, ENUM, the BIT types,
 * OCTET_STRING, GUID, DOMAIN and RECORD. The types with one scalar reading are decoded here. The
 * rest is shown as hex.
 */
function decodeValue(param: DeviceParameter): string {
  const value = param.value;
  if (!Array.isArray(value)) {
    return JSON.stringify(value);
  }

  const name = param.dataTypeName;
  const hexs = hexBytes(value);

  if (name.startsWith('INTEGER')) {
    return `${integerFromBytes(value, { signed: true })}  (0x ${hexs})`;
  }
  if (
    ['UNSIGNED', 'BIT', 'ENUM'].some((prefix) => name.startsWith(prefix)) ||
    ['BYTE', 'WORD', 'DWORD'].includes(name)
  ) {
    return `${integerFromBytes(value)}  (0x ${hexs})`;
  }
  if (name === 'OCTET_STRING') {
    let end = value.length;
    while (end > 0 && (value[end - 1] === 0x00 || value[end - 1] === 0x20)) {
      end--;
    }
    // A byte outside ASCII becomes U+FFFD, the replacement character.
    const text = String.fromCharCode(...value.slice(0, end).map((b) => (b < 0x80 ? b : 0xfffd)));
    return `${JSON.stringify(text)}  (0x ${hexs})`;
  }
  return value.length > 0 ? `0x ${hexs}` : '(empty)';
}

async function main(): Promise<void> {
  const { slave } = cli()
    .description('Read the whole object dictionary of a device with its values and print it.')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  console.log(`reading the object dictionary of slave ${slave}...`);
  const params = (await client.api.initializeDeviceParameters(slave, { readValues: true })).data;

  for (const p of params) {
    console.log(
      `  ${formatHex(p.index)}:${formatHex(p.subindex, 2, false)}  ` +
        `${p.dataTypeName.padEnd(14)}  ${decodeValue(p).padEnd(24)}  ${p.name}`,
    );
  }
  console.log(`${params.length} parameter(s)`);
}

await run(main);
