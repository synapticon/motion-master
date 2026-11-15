// Stream live process data over the monitoring WebSocket.
//
// Creates a monitoring over the HTTP API (POST /api/monitorings), subscribes to its topic on the
// WebSocket, and prints each batch of cycle rows as it arrives. The monitoring is deleted on exit.
// streamMonitoring() only reads a monitoring that exists, so this program creates and deletes it
// with client.api. To record a span and check the rows afterwards, use client.monitoring(), as
// monitoring.ts does.
//
// Opens the fieldbus driver, scans and moves the device to OP first when needed. A value is null
// while its device is not exchanging process data, which starts in SAFEOP.
//
// The defaults sample four objects of the device: Timestamp (0x20F0:00), Position actual value
// (0x6064:00), DC link circuit voltage (0x6079:00) and Drive temperature (0x2031:01). The server
// reads each object from the process image when it is PDO-mapped, and with a background SDO poll
// otherwise. The header line shows which, as (pdo) or (sdo).
//
// Ctrl-C aborts the signal of the stream, so that the loop ends and the program deletes the
// monitoring. Without it, Node.js ends at once and leaves the monitoring on the server.
//
//     pnpm example examples/stream-monitoring.ts --count 10
//     pnpm example examples/stream-monitoring.ts --slave 2 --interval 500     # runs until Ctrl-C

import { AlState, formatHex, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

const TOPIC = 'typescript-example';

/** Returns the time of a row, in UTC with microseconds, such as 19:48:45.043512. */
function rowTime(timestampUs: number): string {
  const seconds = new Date(Math.floor(timestampUs / 1000)).toISOString().slice(11, 19);
  return `${seconds}.${String(timestampUs % 1_000_000).padStart(6, '0')}`;
}

async function main(): Promise<void> {
  const { slave, interval, count } = cli()
    .description('Stream live process data over the monitoring WebSocket.')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .option('--interval <ms>', 'flush cadence in ms (5-2000)', integer, 100)
    .option('--count <n>', 'stop after N batches (0 = until Ctrl-C)', integer, 0)
    .parse()
    .opts<{ slave: number; interval: number; count: number }>();

  // Each parameter is [devicePosition, index, subindex].
  const parameters: [number, number, number][] = [
    [slave, 0x20f0, 0], // Timestamp
    [slave, 0x6064, 0], // Position actual value
    [slave, 0x6079, 0], // DC link circuit voltage
    [slave, 0x2031, 1], // Drive temperature
  ];

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.OP, { positions: [slave] });

  // A monitoring with this topic can remain from a run that was killed, and the server refuses a
  // second create of one topic with 409.
  await client.api.deleteMonitoring(TOPIC).catch(() => {});
  await client.api.createMonitoring({
    topic: TOPIC,
    interval,
    parameters,
  });

  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort(new Error('stopped')));

  try {
    const order = (await client.api.getMonitoring(TOPIC)).data.parameters;
    const header = order
      .map((p) => `${formatHex(p.index)}:${formatHex(p.subindex, 2, false)}(${p.source})`)
      .join(', ');
    console.log(`subscribed to '${TOPIC}' [${header}] -- Ctrl-C to stop`);

    let batches = 0;
    for await (const rows of client.streamMonitoring(TOPIC, { signal: controller.signal })) {
      const [timestampUs, ...values] = rows[rows.length - 1];
      const latest = values.map((v) => String(v)).join(', ');
      console.log(
        `  ${rowTime(timestampUs as number)}  ${String(rows.length).padStart(4)} row(s)  latest=[${latest}]`,
      );
      batches += 1;
      if (count && batches >= count) {
        break;
      }
    }
  } catch (error) {
    if (!controller.signal.aborted) {
      throw error;
    }
    console.log('\nstopping');
  } finally {
    await client.api.deleteMonitoring(TOPIC).catch(() => {});
  }
}

await run(main);
