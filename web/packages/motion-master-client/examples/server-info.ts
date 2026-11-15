// Print the server version, the host, the network adapters and the game loop health.
//
// Reads GET /api/version, GET /api/system-info, GET /api/adapters and GET /api/game-loop. The
// program needs no fieldbus driver, so it opens none and leaves the server as it is.
//
//     pnpm example examples/server-info.ts

import { MotionMasterClient } from '@synapticon/motion-master-client';

const client = await MotionMasterClient.fromConfig();

const { version } = (await client.api.getVersion()).data;
const info = (await client.api.getSystemInfo()).data;
console.log(`Motion Master ${version}`);
console.log(`  host     ${info.hostname}: ${info.osName}, kernel ${info.kernel}`);
console.log(`  cpu      ${info.cpuModel}, ${info.cpuCores} cores, ${info.architecture}`);
console.log(`  memory   ${(info.totalMemoryBytes / 2 ** 30).toFixed(1)} GiB`);

console.log('adapters:');
for (const a of (await client.api.getAdapters()).data) {
  console.log(`  ${a.name.padEnd(20)} ${a.macLinux}  ${a.description}`);
}

// The skipped cycles count since start. A count that climbs steadily means that the period is too
// short for this host.
const loop = (await client.api.getGameLoop()).data;
console.log(
  `game loop: period ${loop.periodUs} us, ${loop.achievedHz.toFixed(1)} of ` +
    `${loop.targetHz.toFixed(0)} Hz, ${loop.skippedCycles} skipped cycle(s), ` +
    `SCHED_FIFO ${loop.schedFifo}, memory locked ${loop.memLocked}`,
);
