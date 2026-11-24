// Find the drives on an Ethernet adapter of the server and list them (GET /api/ethernet/discovery).
//
// The server sends a PROFINET DCP Identify All and an EtherNet/IP ListIdentity request on the
// adapter, collects the answers, and listens for LLDP during that time. It opens no connection to a
// drive. A drive sends LLDP only about every 30 s, so --duration-ms 35000 also finds every drive that
// only LLDP finds. The discovery needs no open fieldbus driver.
//
//     pnpm example examples/discover-ethernet-devices.ts --adapter spoe0
//     pnpm example examples/discover-ethernet-devices.ts --adapter spoe0 --duration-ms 35000

import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { adapter, durationMs } = cli()
    .description(
      'Find the drives on an Ethernet adapter of the server and list them (GET /api/ethernet/discovery).',
    )
    .requiredOption('--adapter <name>', 'name or MAC address of the adapter')
    .option('--duration-ms <ms>', 'how long to collect answers, in ms', integer, 2000)
    .parse()
    .opts<{ adapter: string; durationMs: number }>();

  const client = await MotionMasterClient.fromConfig();

  const result = await client.discoverEthernetDevices(adapter, { durationMs });
  for (const d of result.devices) {
    console.log(
      `  ${(d.ipAddress ?? '-').padEnd(15)}  ${(d.macAddress ?? '-').padEnd(17)}  ` +
        `${(d.firmware ?? '-').padEnd(11)}  ${d.name.padEnd(20)}  found by ${d.foundBy.join(', ')}`,
    );
  }
  for (const warning of result.warnings) {
    console.log(`warning: ${warning}`);
  }
}

await run(main);
