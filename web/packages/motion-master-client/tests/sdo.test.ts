import { describe, expect, it } from 'vitest';
import { integerFromBytes } from '../src/index';

describe('integerFromBytes', () => {
  it('reads little-endian bytes when no byte order is given', () => {
    expect(integerFromBytes([0x37, 0x02])).toBe(567n);
    expect(integerFromBytes(new Uint8Array([0x01, 0x00, 0x00, 0x00]))).toBe(1n);
  });

  it('reads big-endian bytes', () => {
    expect(integerFromBytes([0x02, 0x37], { byteOrder: 'big' })).toBe(567n);
  });

  it('reads a value longer than the 53 bits of a number', () => {
    expect(integerFromBytes([0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff])).toBe(2n ** 64n - 1n);
  });

  it('reads a signed value of an odd width, such as INTEGER24', () => {
    expect(integerFromBytes([0xff, 0xff, 0xff], { signed: true })).toBe(-1n);
    expect(integerFromBytes([0x00, 0x00, 0x80], { signed: true })).toBe(-(2n ** 23n));
    expect(integerFromBytes([0xff, 0xff, 0x7f], { signed: true })).toBe(2n ** 23n - 1n);
    expect(integerFromBytes([0x80, 0x00], { byteOrder: 'big', signed: true })).toBe(-32768n);
  });

  it('reads no bytes as 0', () => {
    expect(integerFromBytes([])).toBe(0n);
    expect(integerFromBytes([], { signed: true })).toBe(0n);
  });
});
