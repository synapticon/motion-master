import { MotionMasterClient, type WebSocketConstructor } from '@synapticon/motion-master-client';
import WebSocket from 'ws';
import { logFetch } from './log-fetch.js';

// The integration suite drives the real client library against a real server, so it tests three
// layers at once: Motion Master, the HTTP/WS contract, and @synapticon/motion-master-client. Node
// < 22 has no global WebSocket, so inject the `ws` package's constructor; the dev cert is
// self-signed, so disable TLS verification on the socket (HTTP already does so via
// NODE_TLS_REJECT_UNAUTHORIZED in global-setup).
export const client = new MotionMasterClient({
  baseUrl: process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:61447',
  wsUrl: process.env.MM_WS_URL ?? 'wss://local.motion-master.synapticon.com:62281',
  WebSocket: WebSocket as unknown as WebSocketConstructor,
  webSocketOptions: { rejectUnauthorized: false },
  customFetch: (input, init) => logFetch('req', input, init),
});
