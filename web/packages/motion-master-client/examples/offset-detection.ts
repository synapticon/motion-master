// Run offset detection on a SOMANET drive (POST .../procedures/offset-detection).
//
// Offset detection runs every measurement a motor needs, in the order they depend on each other:
// open phase detection, phase resistance, phase inductance, pole pair detection, motor phase order
// detection and commutation offset measurement. The program prints each step when its status
// changes, with the value it produced.
//
// This turns the rotor, and it releases the brake for part of the run. The shaft must be free, and
// a load on a vertical axis must be supported first. The server lists every caveat in the
// descriptor of the procedure: GET /api/devices/{slavePosition}/procedures.
//
// Opens the fieldbus driver, scans and moves the device to OP first when needed, because the drive
// enables only while it exchanges process data. Ctrl-C cancels the run. The new phase order and
// commutation offset are not saved to flash. Run the store-parameters procedure to keep them across
// a power cycle.
//
//     pnpm example examples/offset-detection.ts
//     pnpm example examples/offset-detection.ts --slave 2

import { AlState, MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';
import { runToEnd } from './procedures';

const PROCEDURE = 'offset-detection';

async function main(): Promise<void> {
  const { slave } = cli()
    .description('Run offset detection on a SOMANET drive (POST .../procedures/offset-detection).')
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .parse()
    .opts<{ slave: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.OP, { positions: [slave] });

  await runToEnd(client, slave, PROCEDURE);
}

await run(main);
