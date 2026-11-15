// Runs a procedure to its end for an example, and prints it as the examples do. procedures.py is
// the Python twin. The index of the client does not export this file.

import {
  formatStep,
  type MotionMasterClient,
  type ProcedureRequest,
  type ProcedureSnapshot,
  stepChanges,
} from '@synapticon/motion-master-client';

/**
 * Runs a procedure to its end, and prints each step when its status changes, then the error and the
 * final status. Returns the last snapshot when the run succeeded.
 *
 * Ctrl-C cancels the run, because Node.js would otherwise end at once and leave the run going: the
 * program prints "cancelled" on stderr. A run that is cancelled or does not succeed sets exit code 1
 * and returns null.
 */
export async function runToEnd(
  client: MotionMasterClient,
  slavePosition: number,
  name: string,
  params: ProcedureRequest = {},
): Promise<ProcedureSnapshot | null> {
  const controller = new AbortController();
  const onSigint = () => controller.abort(new Error('cancelled'));
  process.once('SIGINT', onSigint);
  let snapshot: ProcedureSnapshot | undefined;
  try {
    for await (const [current, changed] of stepChanges(
      client.runProcedure(slavePosition, name, { params, signal: controller.signal }),
    )) {
      snapshot = current;
      for (const step of changed) {
        console.log(formatStep(step));
      }
    }
  } catch (error) {
    if (!controller.signal.aborted) {
      throw error;
    }
    console.error('\ncancelled');
    process.exitCode = 1;
    return null;
  } finally {
    process.off('SIGINT', onSigint);
  }

  if (snapshot?.error) {
    console.log(`error: ${snapshot.error}`);
  }
  console.log(`${name} ${snapshot?.status}`);
  if (snapshot?.status !== 'succeeded') {
    process.exitCode = 1;
    return null;
  }
  return snapshot;
}
