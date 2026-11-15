// Change the cycle period of the real-time game loop (PUT /api/game-loop).
//
// The change applies at once and lasts until the server stops. It does not change the config file,
// so a restart goes back to the configured period. A skipped cycle count that climbs steadily after
// the change means that the period is too short for this host.
//
// The program needs no fieldbus driver, so it opens none.
//
//     pnpm example examples/set-game-loop-period.ts --period 2000

import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

async function main(): Promise<void> {
  const { period } = cli()
    .description('Change the cycle period of the real-time game loop (PUT /api/game-loop).')
    .requiredOption('--period <us>', 'cycle period in microseconds', integer)
    .parse()
    .opts<{ period: number }>();

  const client = await MotionMasterClient.fromConfig();
  const before = (await client.api.getGameLoop()).data.periodUs;
  await client.api.setGameLoopPeriod({ periodUs: period });
  const after = (await client.api.getGameLoop()).data.periodUs;
  console.log(`game loop period ${before} us -> ${after} us`);
}

await run(main);
