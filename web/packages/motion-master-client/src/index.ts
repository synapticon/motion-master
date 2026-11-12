// Public entry point for @synapticon/motion-master-client.
//
// `src/generated/` is the swagger-typescript-api output — regenerate with `pnpm generate`, never
// hand-edit. Everything else here is the hand-written SDK layer (facade + WebSocket connection).

export * from './generated/http-client'
export * from './generated/data-contracts'
export * from './generated/Api'

export * from './api-error'
export * from './constants'
export * from './web-socket-connection'
export * from './client'

// The objects of a SOMANET device as addresses (generated, see `objects.ts`), the AL states and the
// CiA402 states. `Cia402` comes from `cia402.ts`, which adds the statusword decode to the generated
// addresses.
export { type ObjectAddress, Profile, Somanet } from './objects'
export { Cia402, type Cia402State } from './cia402'
export * from './al-state'

// Pure, framework-agnostic helpers (no DOM/React): hex formatting, CoE SDO value
// encode/decode, SOMANET file-list parsing, the ENI export, and the LAN hostname mapping that lets
// a browser reach a server on another machine under a name the bundled certificate covers.
export * from './lan';
export * from './eni'
export * from './hex'
export * from './sdo'
export * from './mmpd'
export * from './wire-time'
