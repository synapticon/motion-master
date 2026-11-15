// Print the notifications that the server sends over the WebSocket, until Ctrl-C.
//
// Every notification source of the server publishes to one topic, "notifications", so one
// subscription receives every kind of event, also a kind that a later server adds. data.event names
// the kind. The bus health source, for example, sends "short-working-counter" when cycles came back
// with a short working counter.
//
// The program needs no fieldbus driver, so it opens none and leaves the server as it is. A source
// that has nothing to report sends nothing, so on a healthy bus the program can print nothing.
//
//     pnpm example examples/notifications.ts
//     pnpm example examples/notifications.ts --count 5

import { parseArgs } from 'node:util';
import { MotionMasterClient, run } from '@synapticon/motion-master-client';

async function main(): Promise<void> {
  const { values: args } = parseArgs({
    options: { count: { type: 'string', default: '0' } },
  });
  const count = Number(args.count);

  const client = await MotionMasterClient.fromConfig();
  console.log("subscribed to 'notifications' -- Ctrl-C to stop");
  let received = 0;
  for await (const data of client.notifications()) {
    const time =
      new Date().toTimeString().slice(0, 8) + '.' + String(Date.now() % 1000).padStart(3, '0');
    const { event, ...rest } = data;
    console.log(`  ${time}  ${event}  ${JSON.stringify(rest)}`);
    received += 1;
    if (count && received >= count) {
      break;
    }
  }
}

await run(main);
