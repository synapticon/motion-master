// The command line of the examples, with commander, so that each program has --help, required
// options and a fixed list of values, as a Python example has with argparse. The index of the client
// does not export this file.

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

/** Parses an integer option: decimal, or with a 0x, 0o or 0b prefix, as Python's int(text, 0). */
export function integer(text: string): number {
  const value = Number(text);
  if (text.trim() === '' || !Number.isInteger(value)) {
    throw new InvalidArgumentError('Not an integer.');
  }
  return value;
}

/** Parses a number option that can have a fraction, as Python's float(text). */
export function float(text: string): number {
  const value = Number(text);
  if (text.trim() === '' || !Number.isFinite(value)) {
    throw new InvalidArgumentError('Not a number.');
  }
  return value;
}
