# hil/api — HTTP API & WebSocket integration tests

TypeScript integration tests for the Motion Master HTTP API and WebSocket, using [Vitest](https://vitest.dev/). They drive the published client library, [`@synapticon/motion-master-client`](../../web/packages/motion-master-client), against a real server — so a run exercises Motion Master, the HTTP/WS contract, and the client library together.

## Prerequisites

- Node.js 20+ and [pnpm](https://pnpm.io/) 10+
- Docker (used to build and run the server under test)

## Running

This package is part of the repo-root pnpm workspace, so dependencies install once from the root:

```bash
pnpm install                              # from the repo root — first time only
pnpm --filter motion-master-api-tests test   # build image, start container, run tests, stop container
```

Or from inside this directory (pnpm still resolves the workspace):

```bash
cd hil/api
pnpm test
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
| --- | --- | --- |
| `MM_URL` | `https://local.motion-master.synapticon.com:61447` | Base URL of the HTTP API under test |
| `MM_WS_URL` | `wss://local.motion-master.synapticon.com:62281` | URL of the WebSocket |
| `MM_SKIP_DOCKER` | _(unset)_ | Set to `1` to skip Docker management and connect to a running instance |

## Running against an existing instance

```bash
MM_SKIP_DOCKER=1 pnpm --filter motion-master-api-tests test
```

Useful when iterating locally with `./tools/run.sh` already running in another terminal.

## File layout

```text
hil/api/
  src/
    global-setup.ts   ← Docker lifecycle + waitForApi (Vitest globalSetup)
    setup.ts          ← shared MotionMasterClient instance (HTTP + WebSocket)
    log-fetch.ts      ← fetch wrapper that logs requests
  tests/
    system.test.ts        ← version / config / dump
    monitoring.test.ts    ← monitoring routes + WebSocket plumbing
    process-data.test.ts  ← process-image and recorder routes
    auto-tuning.test.ts   ← the auto-tuning child process and its endpoints
  vitest.config.ts
  package.json
  biome.json          ← formatter / linter config
```

`auto-tuning.test.ts` skips its calls when no auto-tuning process is running, rather than failing them. The Docker image the suite builds carries the executable, so the calls do run there; a machine that never had it installed is a supported state, and the status test still checks what the server reports about it.

The typed HTTP client and the WebSocket connection both come from `@synapticon/motion-master-client` (`workspace:*`); there is no generated client checked in here. The client is generated and built in that package — see its README to regenerate from `apps/motion_master/swagger.yml`.
