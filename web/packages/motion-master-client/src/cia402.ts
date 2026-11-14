// The CiA402 objects (0x6xxx) as addresses, the operation modes, and the decode of a statusword.
// The addresses are generated in `objects.ts`. The decode uses the same masks as
// `cia402::decodeState` on the server, and the two clients and the server are tested against one
// table, `libs/node/tests/data/cia402_statusword_states.json`.

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

/**
 * The operation modes, in the encoding of 0x6060 and 0x6061. A value goes into a request body as it
 * is. The values from 0 up are the CiA402 modes. The negative values are the SOMANET modes. Value 5
 * does not exist, because CiA402 reserves it.
 */
export const OperationMode = {
  NO_MODE: 0,
  PROFILE_POSITION: 1,
  VELOCITY: 2,
  PROFILE_VELOCITY: 3,
  PROFILE_TORQUE: 4,
  HOMING: 6,
  INTERPOLATED_POSITION: 7,
  CYCLIC_SYNC_POSITION: 8,
  CYCLIC_SYNC_VELOCITY: 9,
  CYCLIC_SYNC_TORQUE: 10,
  CYCLIC_SYNC_TORQUE_COMMUTATION_ANGLE: 11,
  IMPEDANCE: -6,
  JOINT_TORQUE: -5,
  /** The firmware deprecates this mode. */
  SYSTEM_IDENTIFICATION: -4,
  OPEN_LOOP_FIELD: -3,
  DIAGNOSTICS: -2,
  COGGING_COMPENSATION_RECORDING: -1,
} as const

export type OperationMode = (typeof OperationMode)[keyof typeof OperationMode]

/** The name of an operation mode value, such as `CYCLIC_SYNC_VELOCITY` for 9. */
export function operationModeName(value: number): keyof typeof OperationMode | undefined {
  return (Object.keys(OperationMode) as (keyof typeof OperationMode)[]).find(
    (name) => OperationMode[name] === value,
  )
}

/** The CiA402 objects (0x6xxx) as addresses, such as `Cia402.STATUSWORD`, and `Cia402.state()`. */
export const Cia402 = { ...Cia402Objects, state } as const
