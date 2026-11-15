// Record the EtherCAT frames on the bus adapter for some seconds, and print the summary.
//
// POST /api/captures runs motion-master-capture on the adapter that the bus is on, waits for it to
// end, and answers with the path of the pcap file and its summary. The summary counts, for each kind
// of frame, how many came back later than 2 ms, 20 ms and 100 ms. The request blocks for the whole
// capture. The server runs one capture at a time.
//
// Opens the fieldbus driver first when none is open, because the server records the adapter of the
// open driver. It scans nothing and moves no device.
//
//     pnpm example examples/capture.ts
//     pnpm example examples/capture.ts --seconds 30

import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { seconds } = cli()
    .description(
      'Record the EtherCAT frames on the bus adapter for some seconds, and print the summary.',
    )
    .option('--seconds <n>', 'capture length, 1 to 300', integer, 10)
    .parse()
    .opts<{ seconds: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();

  console.log(`recording for ${seconds} s...`);
  const result = (await client.api.runCapture({ seconds })).data;
  console.log(`pcap on the server: ${result.path}`);
  console.log(JSON.stringify(result.summary, null, 2));
}

await run(main);
