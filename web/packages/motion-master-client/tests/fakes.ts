// The fake Motion Master that the tests share, built on MSW (Mock Service Worker). MSW intercepts the
// global fetch and WebSocket, so the client runs unchanged. setup.ts starts the server for every test
// file and removes each test's handlers after it.

import { setupServer } from 'msw/node';
import { HttpResponse, http } from 'msw/http';
import { ws } from 'msw/ws';
import { MotionMasterClient } from '../src/index';

/** The addresses the fake server answers on. */
export const HTTP_URL = 'http://motion-master.test';
export const WS_URL = 'ws://motion-master.test';

export const server = setupServer();

/** One request that the fake server received. */
export interface RecordedRequest {
  method: string;
  path: string;
  query: string;
  body: unknown;
}

/** A route's answer: an HTTP status and a JSON body, or no body. */
export type Answer = [status: number, body?: unknown];

/**
 * Answers the HTTP API with `routes`, which map `METHOD /path` to a function returning the answer.
 * A request with no route gets 404. Returns a client for the fake server, every request in the
 * order it came, and `calls()`, the requests as `METHOD /path?query`.
 */
export function fakeServer(
  routes: Record<string, (body: unknown) => Answer>,
  options: ConstructorParameters<typeof MotionMasterClient>[0] = {},
) {
  const requests: RecordedRequest[] = [];
  server.use(
    http.all(`${HTTP_URL}/*`, async ({ request }) => {
      const url = new URL(request.url);
      const text = await request.text();
      const body = text ? JSON.parse(text) : undefined;
      requests.push({ method: request.method, path: url.pathname, query: url.search, body });
      const route = routes[`${request.method} ${url.pathname}`];
      const [status, answer] = route ? route(body) : [404, { error: 'no route' }];
      return answer === undefined
        ? new HttpResponse(null, { status })
        : HttpResponse.json(answer as never, { status });
    }),
  );
  const client = new MotionMasterClient({ baseUrl: HTTP_URL, wsUrl: WS_URL, ...options });
  const calls = () => requests.map((r) => `${r.method} ${r.path}${r.query}`);
  return { client, requests, calls };
}

/**
 * Answers the WebSocket. `sent` collects every message a client sends, parsed, and `onMessage`
 * sees each one as it comes. `send` delivers a message to every connected client, and `close` ends
 * every connection, as a server that goes away.
 */
export function fakeSocket(onMessage: (message: unknown) => void = () => {}) {
  const link = ws.link(WS_URL);
  const sent: unknown[] = [];
  let connections = 0;
  server.use(
    link.addEventListener('connection', ({ client }) => {
      connections += 1;
      client.addEventListener('message', (event) => {
        const message: unknown = JSON.parse(String(event.data));
        sent.push(message);
        onMessage(message);
      });
    }),
  );
  return {
    sent,
    connections: () => connections,
    send: (message: unknown) => link.broadcast(JSON.stringify(message)),
    close: () => {
      for (const client of link.clients) {
        client.close();
      }
    },
  };
}
