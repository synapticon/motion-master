// Restore the default parameter values of a device (procedure restore-default-parameters).
//
// The procedure writes the "load" signature to 0x1011 for the group you choose, and polls the
// device until it reports the restore complete. The live values of that group are replaced, so
// anything not stored is gone. The defaults go into volatile memory: run store-parameters.ts to keep
// them, and some devices apply them only after a reset or a power cycle. Ctrl-C stops the wait, not
// the restore, because the device already has the command.
//
// Opens the fieldbus driver, scans and moves the device to PREOP first when needed. A device in
// SAFEOP or OP stays there.
//
//     pnpm example examples/restore-default-parameters.ts
//     pnpm example examples/restore-default-parameters.ts --group application --slave 2

import {
  AlState,
  formatStep,
  MotionMasterClient,
  type ProcedureRequest,
  type ProcedureSnapshot,
  run,
  stepChanges,
} from '@synapticon/motion-master-client';
import { Option } from 'commander';
import { cli, integer } from './options';

const PROCEDURE = 'restore-default-parameters';

// The 0x1011 groups that the procedure can restore.
const GROUPS: NonNullable<ProcedureRequest['group']>[] = [
  'all',
  'communication',
  'application',
  'manufacturer',
];

async function main(): Promise<void> {
  const { slave, group } = cli()
    .description(
      'Restore the default parameter values of a device (procedure restore-default-parameters).',
    )
    .option('--slave <n>', 'the position of the device on the bus', integer, 1)
    .addOption(
      new Option('--group <group>', 'the 0x1011 group to restore').choices(GROUPS).default('all'),
    )
    .parse()
    .opts<{ slave: number; group: NonNullable<ProcedureRequest['group']> }>();
  const params = { group };

  const client = await MotionMasterClient.fromConfig();
  await client.init();
  await client.ensureState(AlState.PREOP, { positions: [slave] });

  // Ctrl-C aborts the signal, so that runProcedure() cancels the run before the program ends.
  const controller = new AbortController();
  process.once('SIGINT', () => controller.abort(new Error('cancelled')));

  let snapshot: ProcedureSnapshot | undefined;
  try {
    for await (const [current, changed] of stepChanges(
      client.runProcedure(slave, PROCEDURE, { params, signal: controller.signal }),
    )) {
      snapshot = current;
      for (const step of changed) {
        console.log(formatStep(step));
      }
    }
  } catch (e) {
    if (controller.signal.aborted) {
      console.error('\ncancelled');
      process.exitCode = 1;
      return;
    }
    throw e;
  }

  if (snapshot?.error) {
    console.log(`error: ${snapshot.error}`);
  }
  console.log(`${PROCEDURE} ${snapshot?.status}`);
  if (snapshot?.status !== 'succeeded') {
    process.exitCode = 1;
  }
}

await run(main);
