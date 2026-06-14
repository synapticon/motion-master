// Public entry point for @synapticon/motion-master-client.
//
// `src/generated/` is the swagger-typescript-api output — regenerate with `pnpm generate`, never
// hand-edit. Everything else here is the hand-written SDK layer (facade + WebSocket connection).

export * from './generated/http-client'
export * from './generated/data-contracts'
export * from './generated/Api'

export * from './constants'
export * from './web-socket-connection'
export * from './client'

// Pure, framework-agnostic helpers (no DOM/React): hex formatting, CoE SDO value
// encode/decode, and SOMANET file-list parsing.
export * from './hex'
export * from './sdo'
export * from './somanet'
