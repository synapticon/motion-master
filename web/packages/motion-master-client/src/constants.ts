// Default endpoints for a Motion Master instance running on the same machine as the client. The
// bundled Let's Encrypt cert is for `local.motion-master.synapticon.com`, which resolves to
// 127.0.0.1, so these work out of the box for a localhost install. Override both via
// `MotionMasterClient` options for a remote/LAN deployment.

/** The host name of a server on this computer, which the bundled certificate covers. */
export const DEFAULT_HOST = 'local.motion-master.synapticon.com';

/** The port of the HTTP API. */
export const DEFAULT_HTTP_PORT = 61447;

/**
 * The port of the WebSocket. It has its own port and loop, separate from the HTTP API, so that a
 * slow HTTP request cannot stall the monitoring and notification streams.
 */
export const DEFAULT_WS_PORT = 62281;

/** The default address of the HTTP API. */
export const API_BASE_URL = `https://${DEFAULT_HOST}:${DEFAULT_HTTP_PORT}`;

/** The default address of the WebSocket. */
export const WS_URL = `wss://${DEFAULT_HOST}:${DEFAULT_WS_PORT}`;
