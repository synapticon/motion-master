// Default endpoints for a Motion Master instance running on the same machine as the client. The
// bundled Let's Encrypt cert is for `local.motion-master.synapticon.com`, which resolves to
// 127.0.0.1, so these work out of the box for a localhost install. Override both via
// `MotionMasterClient` options for a remote/LAN deployment.
export const API_BASE_URL = 'https://local.motion-master.synapticon.com:61447'

// The realtime WebSocket runs on its own port/loop (separate from the HTTP API) so a slow HTTP
// request can never stall the monitoring/notification stream.
export const WS_URL = 'wss://local.motion-master.synapticon.com:62281'
