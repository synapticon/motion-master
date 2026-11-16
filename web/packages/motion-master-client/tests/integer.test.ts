import { describe, expect, it } from 'vitest';
import { parseInteger, parseBigInt } from '../src/index';

describe('parseInteger', () => {
  it("reads an integer as Python's int(text, 0) does", () => {
    expect(parseInteger('42')).toBe(42);
    expect(parseInteger(' -42 ')).toBe(-42);
    expect(parseInteger('+7')).toBe(7);
    expect(parseInteger('0x6041')).toBe(0x6041);
    expect(parseInteger('-0x10')).toBe(-16);
    expect(parseInteger('0o17')).toBe(15);
    expect(parseInteger('0b101')).toBe(5);
    expect(parseInteger('0')).toBe(0);
    expect(parseInteger('00')).toBe(0);
  });

  it('refuses text that is not such an integer', () => {
    for (const text of ['', ' ', '010', '1.0', '1e3', '0x', '12abc', '0xG', '- 1', '1_000']) {
      expect(parseInteger(text), text).toBeNull();
    }
  });

  it('refuses an integer that a number cannot hold exactly', () => {
    expect(parseInteger('9007199254740991')).toBe(Number.MAX_SAFE_INTEGER);
    expect(parseInteger('9007199254740992')).toBeNull();
  });

  it('refuses an integer outside min and max', () => {
    expect(parseInteger('255', { min: 0, max: 255 })).toBe(255);
    expect(parseInteger('0x100', { min: 0, max: 255 })).toBeNull();
    expect(parseInteger('-1', { min: 0 })).toBeNull();
  });
});

describe('parseBigInt', () => {
  it('keeps every digit of a 64-bit value', () => {
    expect(parseBigInt('0xFFFFFFFFFFFFFFFF')).toBe(0xffffffffffffffffn);
    expect(parseBigInt('-9223372036854775808')).toBe(-9223372036854775808n);
    expect(parseBigInt('nope')).toBeNull();
  });
});
