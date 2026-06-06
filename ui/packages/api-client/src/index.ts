// Generated API client — run `pnpm generate-api` to populate src/generated/
export * from './generated/Api'
export * from './generated/data-contracts'

export const API_BASE_URL = 'https://local.motion-master.synapticon.com:61447'
// The realtime WebSocket runs on its own port/loop (separate from the HTTP API) so a slow HTTP
// request can never stall the monitoring/notification stream.
export const WS_URL = 'wss://local.motion-master.synapticon.com:62281'
