import { afterAll, afterEach, beforeAll } from 'vitest';
import { server } from './fakes';

// A request or a WebSocket connection that no handler expects fails the test, rather than reaching
// the network.
beforeAll(() => server.listen({ onUnhandledFrame: 'error' }));
afterEach(() => server.resetHandlers());
afterAll(() => server.close());
