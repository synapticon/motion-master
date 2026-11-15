// Helpers for the snapshots of a procedure run, as `MotionMasterClient.runProcedure` yields them.

import type { ProcedureSnapshot, ProgressStep } from './generated/data-contracts';

/**
 * Yields each snapshot of a run from `runProcedure()`, with the steps whose status changed.
 *
 * Every snapshot is yielded, as `[snapshot, changed]`. `changed` lists the steps whose status is
 * not the one they had in the snapshot before, and it can be empty. A step not reached yet is idle,
 * so it is listed only once it starts. The last snapshot carries the final status of the run.
 * Leaving the loop early closes `run`, which cancels a run that is still in progress.
 */
export async function* stepChanges(
  run: AsyncGenerator<ProcedureSnapshot, void, undefined>,
): AsyncGenerator<[ProcedureSnapshot, ProgressStep[]], void, undefined> {
  // A step that starts and ends between two polls shows only once, already finished, so a step is
  // compared with its last status, not with the poll before.
  const seen = new Map<string, string>();
  // Leaving this loop early closes `run`, because `for await` closes the generator it reads.
  for await (const snapshot of run) {
    const changed: ProgressStep[] = [];
    for (const step of snapshot.steps) {
      if ((seen.get(step.id) ?? 'idle') !== step.status) {
        seen.set(step.id, step.status);
        changed.push(step);
      }
    }
    yield [snapshot, changed];
  }
}

/** Returns one line for a step of a procedure: its id, its status, and its value or error. */
export function formatStep(step: ProgressStep): string {
  let line = `  ${step.id.padEnd(36)} ${step.status}`;
  if ('value' in step) {
    line += `  ${JSON.stringify(step.value)}`;
  }
  if ('error' in step) {
    line += `  ${step.error}`;
  }
  return line;
}
