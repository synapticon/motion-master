// Print the ESC error counters and the distributed clock sync of the devices.
//
// Reads GET /api/devices/diagnostics and GET /api/dc-sync. The counters come live from the ESC
// (EtherCAT Slave Controller) registers of each device. Each counter is 8 bits wide and stops at
// 255. Only a power cycle or a write to the register clears it, so a counter that is not zero says
// that errors happened, not when.
//
// Opens the fieldbus driver and scans first when needed, and moves no device.
//
//     pnpm example examples/diagnostics.ts
//     pnpm example examples/diagnostics.ts --slave 2

import { MotionMasterClient, run } from '@synapticon/motion-master-client';
import { cli, integer } from './options';

const PORT_COUNTERS = ['rxError', 'invalidFrame', 'forwardedError', 'lostLink'] as const;

async function main(): Promise<void> {
  const { slave } = cli()
    .description('Print the ESC error counters and the distributed clock sync of the devices.')
    .option('--slave <n>', 'one device position (default: every device)', integer)
    .parse()
    .opts<{ slave?: number }>();

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureScanned();

  const query = slave ? { positions: String(slave) } : undefined;
  for (const d of (await client.api.getDeviceDiagnostics(query)).data) {
    console.log(
      `device ${d.slavePosition} ${d.deviceName}: processing unit errors ` +
        `${d.processingUnitError}, PDI errors ${d.pdiError}, PDI watchdog ` +
        `${d.pdiWatchdog}, process data watchdog ${d.processDataWatchdog}`,
    );
    d.ports.forEach((port, n) => {
      if (!port.linkUp) {
        return;
      }
      const counters = PORT_COUNTERS.map((c) => `${c} ${port[c]}`).join(', ');
      console.log(`  port ${n}: link up, ${counters}`);
    });
  }

  console.log('distributed clock:');
  for (const d of (await client.api.getDcSync(query)).data) {
    if (!d.dcCapable) {
      console.log(`  device ${d.slavePosition}: not DC-capable`);
      continue;
    }
    const role = d.referenceClock ? 'reference clock' : 'disciplined';
    console.log(
      `  device ${d.slavePosition}: ${role}, system time difference ` +
        `${d.systemTimeDifference} ns, propagation delay ${d.propagationDelay} ns`,
    );
  }
}

await run(main);
