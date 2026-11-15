// How the client addresses one parameter of one device, and how it names an object. Shared by the
// helpers that take a list of parameters, such as `MotionMasterClient.monitoring`.

import { Cia402 } from './cia402';
import { formatHex } from './hex';
import { type ObjectAddress, Profile, Somanet } from './objects';

/**
 * One parameter of one device: the device position, then the index and the subindex, or the
 * device position and an address such as `Cia402.STATUSWORD`.
 */
export type DeviceParameterAddress =
  | readonly [devicePosition: number, index: number, subindex: number]
  | readonly [devicePosition: number, address: ObjectAddress<unknown>];

/** Returns a parameter as `[devicePosition, index, subindex]`, the form the API takes. */
export function parameterTriple(parameter: DeviceParameterAddress): [number, number, number] {
  if (parameter.length === 3) {
    return [parameter[0], parameter[1], parameter[2]];
  }
  return [parameter[0], parameter[1].index, parameter[1].subindex];
}

let objectNames: Map<string, string> | undefined;

/** Returns the generated name of an object, such as STATUSWORD, or 0xIIII:SS when it has none. */
export function objectName(index: number, subindex: number): string {
  if (objectNames === undefined) {
    // The first name wins, in the order of the Python client: Profile, Cia402, Somanet.
    objectNames = new Map();
    for (const group of [Profile, Cia402, Somanet]) {
      for (const [name, value] of Object.entries(group)) {
        if (typeof value === 'object' && value !== null && 'index' in value) {
          const key = `${value.index}:${value.subindex}`;
          if (!objectNames.has(key)) {
            objectNames.set(key, name);
          }
        }
      }
    }
  }
  return (
    objectNames.get(`${index}:${subindex}`) ??
    `${formatHex(index)}:${formatHex(subindex, 2, false)}`
  );
}
