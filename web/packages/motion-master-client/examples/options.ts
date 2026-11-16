// The command line of the examples, with commander, so that each program has --help, required
// options and a fixed list of values, as a Python example has with argparse. options.py is the
// Python twin. The index of the client does not export this file.

import {
  AlState,
  parseInteger,
  parseBigInt,
  type TargetAlState,
} from '@synapticon/motion-master-client';
import { Command, InvalidArgumentError } from 'commander';

/**
 * Returns a command line that ends a usage error with exit code 2, as argparse does. commander ends
 * it with 1. --help still ends with 0.
 */
export function cli(): Command {
  return new Command().exitOverride((error) => {
    process.exit(error.exitCode === 0 ? 0 : 2);
  });
}

/**
 * Parses an integer option as Python's int(text, 0) does: decimal, or with a 0x, 0o or 0b prefix,
 * with an optional sign. A decimal with a leading zero, a fraction and an exponent are refused, and
 * so is an integer that a JavaScript number cannot hold exactly.
 */
export function integer(text: string): number {
  const value = parseInteger(text);
  if (value !== null) {
    return value;
  }
  if (parseBigInt(text) === null) {
    throw new InvalidArgumentError('Not an integer.');
  }
  throw new InvalidArgumentError('Beyond 2^53 - 1, which a JavaScript number cannot hold.');
}

/** Parses a number option that can have a fraction, as Python's float(text). */
export function float(text: string): number {
  const value = Number(text);
  if (text.trim() === '' || !Number.isFinite(value)) {
    throw new InvalidArgumentError('Not a number.');
  }
  return value;
}

/** Parses a list of bytes, such as 0x10,0x27 or 16,39: integers from 0 to 255, comma-separated. */
export function byteList(text: string): number[] {
  return text.split(',').map((part) => {
    const value = parseInteger(part, { min: 0, max: 255 });
    if (value === null) {
      throw new InvalidArgumentError(`${part.trim()} is not a byte, 0 to 255.`);
    }
    return value;
  });
}

/** The AL states that a device can be moved to, by their names in lower case: init to op. */
export const AL_STATES = Object.fromEntries(
  Object.entries(AlState)
    .filter(([name]) => name !== 'UNKNOWN')
    .map(([name, value]) => [name.toLowerCase(), value]),
) as Record<string, TargetAlState>;
