// Close the fieldbus driver and clear the device list (POST /api/reset).
//
// The server unpublishes the process image, frees the recorder, removes every device and closes the
// network interface. It commands no AL state, so the devices stay in the state they are in. The
// server then is as it was at start, with no driver open.
//
// A reset with no driver open also succeeds, so the program runs again with the same result. The
// next program that needs a bus opens the driver and scans again.
//
//     pnpm example examples/reset.ts

import { MotionMasterClient, run } from '@synapticon/motion-master-client';

async function main(): Promise<void> {
  const client = await MotionMasterClient.fromConfig();
  await client.reset();
  console.log('reset: the fieldbus driver is closed and the device list is empty');
}

await run(main);
