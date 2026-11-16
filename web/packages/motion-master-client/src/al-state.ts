// The AL states, in the encoding of the AL control register that the API uses. A value goes into
// a request body as it is, and an `alState` from a response is one of these values. The name is
// the label, such as `PREOP`. The server reports `UNKNOWN` for a device that does not answer.

export const AlState = {
  UNKNOWN: 0,
  INIT: 1,
  PREOP: 2,
  BOOT: 3,
  SAFEOP: 4,
  OP: 8,
} as const;

export type AlState = (typeof AlState)[keyof typeof AlState];

/**
 * Whether a device in this AL state answers mailbox requests, such as an SDO: true in PREOP,
 * SAFEOP and OP, also with the error bit set. INIT has no mailbox, and the mailbox of BOOT carries
 * FoE only. The server decides the same way.
 */
export function mailboxActive(alState: number): boolean {
  return alState === AlState.PREOP || alState === AlState.SAFEOP || alState === AlState.OP;
}

/** The name of an AL state value, such as `PREOP` for 2. */
export function alStateName(value: number): keyof typeof AlState | undefined {
  return (Object.keys(AlState) as (keyof typeof AlState)[]).find((name) => AlState[name] === value);
}
