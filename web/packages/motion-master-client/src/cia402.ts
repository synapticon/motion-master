// The CiA402 objects (0x6xxx) as addresses, and the decode of a statusword. The addresses are
// generated in `objects.ts`. The decode uses the same masks as `cia402::decodeState` on the server,
// and the two clients and the server are tested against one table,
// `libs/node/tests/data/cia402_statusword_states.json`.

import type { Cia402Status } from './generated/data-contracts'
import { Cia402 as Cia402Objects } from './objects'

/** A CiA402 state, by the name the API uses. */
export type Cia402State = Cia402Status['state']

/**
 * Returns the CiA402 state that a statusword (0x6041) reports. A pattern that matches no state is
 * `NotReadyToSwitchOn`. Meant for recorded rows, where the server sends the raw statusword.
 */
function state(statusword: number): Cia402State {
  if ((statusword & 0x4f) === 0x40) {
    return 'SwitchOnDisabled'
  }
  if ((statusword & 0x6f) === 0x21) {
    return 'ReadyToSwitchOn'
  }
  if ((statusword & 0x6f) === 0x23) {
    return 'SwitchedOn'
  }
  if ((statusword & 0x6f) === 0x27) {
    return 'OperationEnabled'
  }
  if ((statusword & 0x6f) === 0x07) {
    return 'QuickStopActive'
  }
  if ((statusword & 0x4f) === 0x0f) {
    return 'FaultReactionActive'
  }
  if ((statusword & 0x4f) === 0x08) {
    return 'Fault'
  }
  return 'NotReadyToSwitchOn'
}

/** The CiA402 objects (0x6xxx) as addresses, such as `Cia402.STATUSWORD`, and `Cia402.state()`. */
export const Cia402 = { ...Cia402Objects, state } as const
