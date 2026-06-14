# Motion Master — web

All of Motion Master's browser/Node TypeScript lives here:

- [`apps/motion-master`](apps/motion-master) — the Motion Master PWA (React + Vite + Tailwind).
- [`packages/motion-master-client`](packages/motion-master-client) — `@synapticon/motion-master-client`, the published, isomorphic SDK (the generated HTTP API client + the WebSocket connection). The app consumes it as a `workspace:*` dependency.

These are members of a single pnpm workspace **rooted at the repository root** (`pnpm-workspace.yaml`), alongside `hil/api`. So pnpm commands run from the repo root, not from `web/`.

## Prerequisites

- Node.js 20+
- [pnpm](https://pnpm.io/) 10+

## Running the web app

From the **repository root**:

```bash
pnpm install        # once (or after dependency changes)
pnpm generate-api   # once, or whenever apps/motion_master/swagger.yml changes
pnpm dev            # start the Vite dev server for the motion-master app
```

- `pnpm dev` is shorthand for `pnpm --filter motion-master dev`.
- **`pnpm generate-api` is a required first step on a fresh clone.** The app imports `@synapticon/motion-master-client`, which resolves to the client's TypeScript *source* (`packages/motion-master-client/src/index.ts` → `src/generated/`). That `generated/` directory is git-ignored and only exists after generation, so the dev server can't resolve it until you run this once. You do **not** need to build the client (`tsc`) for development — Vite transpiles its source directly; the build step is only for publishing.
- The app talks to a running server at `https://local.motion-master.synapticon.com:61447` (HTTP API) and `wss://local.motion-master.synapticon.com:62281` (WebSocket). Start the server separately with `./tools/run.sh`; the endpoint is configurable on the app's Connection page.

## Building

```bash
pnpm build          # generate the client, then build the app (dist/ in apps/motion-master)
pnpm build:client   # generate + tsc-build the client library only (the published ESM artifact)
```

## Tests

The HTTP/WebSocket integration tests (which drive the client library against a real server in Docker) live in [`hil/api`](../hil/api):

```bash
pnpm --filter motion-master-api-tests test
```
