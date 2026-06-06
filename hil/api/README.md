# hil/api — HTTP API & WebSocket integration tests

TypeScript integration tests for the Motion Master HTTP API and monitoring WebSocket, using [Vitest](https://vitest.dev/).

## Prerequisites

- Node.js 20+
- Docker (used to build and run the server under test)

## Running

```bash
cd hil/api
npm install          # first time only
npm test             # build image, start container, run tests, stop container
```

The global setup (`src/global-setup.ts`) manages the full Docker lifecycle:

1. Removes any leftover `motion-master-api-test` container from a previous failed run.
2. Builds the `motion-master` image from the repo root (`docker build`).
3. Starts the container with `docker run -d --rm --network host`.
4. Polls `/api/version` until the server is ready (up to 60 s).
5. After all tests complete, stops the container (`docker stop`); `--rm` ensures it is removed automatically.

`--network host` is required because Motion Master binds to `127.0.0.1:61447` — Docker's default bridge NAT would never reach a loopback listener.

## Environment variables

| Variable | Default | Description |
|---|---|---|
| `MM_URL` | `https://local.motion-master.synapticon.com:61447` | Base URL of the server under test |
| `MM_SKIP_DOCKER` | _(unset)_ | Set to `1` to skip Docker management and connect to a running instance |

Copy `.env.example` to `.env` and adjust as needed:

```bash
cp .env.example .env
```

## Running against an existing instance

```bash
MM_SKIP_DOCKER=1 npm test
```

Useful when iterating locally with `./tools/run.sh` already running in another terminal.

## File layout

```
hil/api/
  src/
    global-setup.ts   ← Docker lifecycle + waitForApi (Vitest globalSetup)
    setup.ts          ← typed API client shared by tests
    log-fetch.ts      ← fetch wrapper that logs requests
    mm-api.ts         ← generated typed client (from swagger.yml)
  tests/
    system.test.ts    ← integration tests
  vitest.config.ts
  package.json
  biome.json          ← formatter / linter config
```

## Regenerating the API client

```bash
npm run generate:api
```

Reads `apps/motion_master/swagger.yml` and overwrites `src/mm-api.ts`.
