import { describe, expect, it } from 'vitest';
import {
  procedureFailure,
  procedureParameterValue,
  stepValue,
  type ProcedureParameter,
  type ProcedureSnapshot,
} from '../src/index';

function snapshot(fields: Partial<ProcedureSnapshot>): ProcedureSnapshot {
  return { status: 'running', steps: [], ...fields } as ProcedureSnapshot;
}

function parameter(fields: Partial<ProcedureParameter>): ProcedureParameter {
  return { name: 'p', title: 'P', description: '', type: 'string', required: false, ...fields };
}

describe('stepValue', () => {
  it('returns the value of the step with the id, or undefined', () => {
    const s = snapshot({
      steps: [
        { id: 'read', status: 'succeeded', value: { gains: 1 } },
        { id: 'write', status: 'running' },
      ],
    });
    expect(stepValue<{ gains: number }>(s, 'read')).toEqual({ gains: 1 });
    expect(stepValue(s, 'write')).toBeUndefined();
    expect(stepValue(s, 'none')).toBeUndefined();
    expect(stepValue(undefined, 'read')).toBeUndefined();
  });
});

describe('procedureFailure', () => {
  const steps = [{ id: 'a', status: 'failed' as const, error: 'step error' }];

  it('says nothing while the run goes on or after it succeeded', () => {
    expect(procedureFailure(snapshot({ steps }))).toBeUndefined();
    expect(procedureFailure(snapshot({ status: 'succeeded' }))).toBeUndefined();
    expect(procedureFailure(undefined)).toBeUndefined();
  });

  it('prefers the error of the run, then of a step, then the status', () => {
    expect(procedureFailure(snapshot({ status: 'failed', error: 'run error', steps }))).toBe(
      'run error',
    );
    expect(procedureFailure(snapshot({ status: 'failed', steps }))).toBe('step error');
    expect(procedureFailure(snapshot({ status: 'cancelled' }))).toBe('cancelled');
  });
});

describe('procedureParameterValue', () => {
  it('converts an integer within its range', () => {
    const p = parameter({ type: 'integer', minValue: 0, maxValue: 255 });
    expect(procedureParameterValue(p, ' 0x10 ')).toBe(16);
    expect(() => procedureParameterValue(p, '256')).toThrow('Not between 0 and 255.');
    expect(() => procedureParameterValue(p, '1e3')).toThrow('Not a whole number');
  });

  it('converts a boolean and an enum', () => {
    expect(procedureParameterValue(parameter({ type: 'boolean' }), 'TRUE')).toBe(true);
    expect(() => procedureParameterValue(parameter({ type: 'boolean' }), 'yes')).toThrow(
      'Expected true or false.',
    );
    const e = parameter({
      type: 'enum',
      options: [
        { value: 1, title: 'one' },
        { value: 'two', title: 'two' },
      ],
    });
    expect(procedureParameterValue(e, '1')).toBe(1);
    expect(() => procedureParameterValue(e, '3')).toThrow('Expected one of 1, two.');
  });

  it('converts bytes separated by commas or spaces, with the declared length', () => {
    const p = parameter({ type: 'byteArray', length: 3 });
    expect(procedureParameterValue(p, '0x15, 0 255')).toEqual([0x15, 0, 255]);
    expect(() => procedureParameterValue(p, '1,2')).toThrow('Expected 3 bytes, got 2.');
    expect(() => procedureParameterValue(p, '1,2,256')).toThrow('256 is not a byte, 0 to 255.');
  });

  it('splits a list on commas and line breaks, and keeps an empty list', () => {
    const p = parameter({ type: 'stringArray' });
    expect(procedureParameterValue(p, 'a, b\nc\r\n\n')).toEqual(['a', 'b', 'c']);
    expect(procedureParameterValue(p, '')).toEqual([]);
  });

  it('leaves out an empty value, and keeps text as it is', () => {
    expect(procedureParameterValue(parameter({ type: 'integer' }), '  ')).toBeUndefined();
    expect(procedureParameterValue(parameter({ type: 'string' }), '')).toBeUndefined();
    expect(procedureParameterValue(parameter({ type: 'string' }), ' a b ')).toBe(' a b ');
  });
});
