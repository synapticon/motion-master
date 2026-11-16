// Helpers for the snapshots of a procedure run, as `MotionMasterClient.runProcedure` yields them.

import type {
  ProcedureParameter,
  ProcedureSnapshot,
  ProgressStep,
} from './generated/data-contracts';
import { parseInteger } from './integer';

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

/**
 * Returns the value that one step of a run produced, such as `stepValue(snapshot, 'compute')`.
 * The shape depends on the procedure and the step, as the API documents it. Returns `undefined`
 * when there is no snapshot, when the run has no step with this id, and when the step has
 * produced nothing so far. `T` is the shape the caller expects; nothing checks it.
 */
export function stepValue<T = unknown>(
  snapshot: ProcedureSnapshot | undefined,
  id: string,
): T | undefined {
  return snapshot?.steps.find((step) => step.id === id)?.value as T | undefined;
}

/**
 * Returns why a run failed or was cancelled: the error of the run, else the error of a step, else
 * the status itself. Returns `undefined` while the run goes on and after it succeeded.
 */
export function procedureFailure(snapshot: ProcedureSnapshot | undefined): string | undefined {
  if (snapshot?.status !== 'failed' && snapshot?.status !== 'cancelled') {
    return undefined;
  }
  return snapshot.error ?? snapshot.steps.find((step) => step.error)?.error ?? snapshot.status;
}

/**
 * Converts the text of one parameter of a procedure to the value its request takes, by the type
 * that the descriptor of the procedure declares:
 *
 * - `integer`: decimal, or hex, octal or binary, as `parseInteger()` reads it, within `minValue`
 *   and `maxValue`.
 * - `boolean`: `true` or `false`, in any case.
 * - `enum`: the value of one of the options, as text.
 * - `byteArray`: bytes from 0 to 255, separated by commas or white space, exactly `length` of
 *   them when the descriptor gives a length.
 * - `stringArray`: the items, separated by commas or line breaks. An item can hold neither.
 * - `string` and `file`: the text as it is. A file goes as its content in base64, which the caller
 *   reads, because a browser and Node.js read a file differently.
 *
 * Returns `undefined` for empty text, which leaves the parameter out of the request so that the
 * server uses its default. An empty `stringArray` is `[]`, an empty list. Throws an `Error` that
 * says what is wrong with the text.
 */
export function procedureParameterValue(parameter: ProcedureParameter, text: string): unknown {
  if (parameter.type === 'stringArray') {
    return text
      .split(/[,\n]/)
      .map((item) => item.trim())
      .filter((item) => item.length > 0);
  }
  if (parameter.type === 'string' || parameter.type === 'file') {
    return text === '' ? undefined : text;
  }
  const trimmed = text.trim();
  if (trimmed === '') {
    return undefined;
  }
  switch (parameter.type) {
    case 'integer': {
      const value = parseInteger(trimmed);
      if (value === null) {
        throw new Error('Not a whole number. Write it in decimal, or in hex with 0x.');
      }
      const { minValue: min, maxValue: max } = parameter;
      if ((min !== undefined && value < min) || (max !== undefined && value > max)) {
        throw new Error(`Not between ${min ?? '-∞'} and ${max ?? '∞'}.`);
      }
      return value;
    }
    case 'boolean': {
      const lower = trimmed.toLowerCase();
      if (lower !== 'true' && lower !== 'false') {
        throw new Error('Expected true or false.');
      }
      return lower === 'true';
    }
    case 'enum': {
      const options = parameter.options ?? [];
      const option = options.find((o) => String(o.value) === trimmed);
      if (option === undefined) {
        throw new Error(`Expected one of ${options.map((o) => String(o.value)).join(', ')}.`);
      }
      return option.value;
    }
    case 'byteArray': {
      const tokens = trimmed.split(/[\s,]+/).filter((token) => token.length > 0);
      if (parameter.length !== undefined && tokens.length !== parameter.length) {
        throw new Error(`Expected ${parameter.length} bytes, got ${tokens.length}.`);
      }
      return tokens.map((token) => {
        const value = parseInteger(token, { min: 0, max: 255 });
        if (value === null) {
          throw new Error(`${token} is not a byte, 0 to 255.`);
        }
        return value;
      });
    }
  }
  return trimmed;
}
