import { describe, expect, it } from 'vitest';
import { AlState, alStateName, mailboxActive } from '../src/index';

describe('AL states', () => {
  it('names a state by its value', () => {
    expect(alStateName(AlState.PREOP)).toBe('PREOP');
    expect(alStateName(0)).toBe('UNKNOWN');
    expect(alStateName(5)).toBeUndefined();
  });

  it('says the mailbox answers in PREOP, SAFEOP and OP only', () => {
    const active = [0, 1, 2, 3, 4, 8].filter((state) => mailboxActive(state));
    expect(active).toEqual([AlState.PREOP, AlState.SAFEOP, AlState.OP]);
  });
});
