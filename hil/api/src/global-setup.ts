// Vitest global setup — runs once before the entire test suite.
// Motion Master must already be running before invoking vitest.
// Start it with: ./tools/run.sh  (from the motion-master repo root)

const mmUrl = process.env.MM_URL ?? 'https://local.motion-master.synapticon.com:8443';

// Self-signed cert from ./tools/run.sh — skip TLS verification in tests.
process.env['NODE_TLS_REJECT_UNAUTHORIZED'] = '0';

async function waitForApi(timeoutMs = 30_000): Promise<void> {
  const deadline = Date.now() + timeoutMs;
  while (Date.now() < deadline) {
    try {
      const res = await fetch(`${mmUrl}/api/version`);
      if (res.ok) {
        return;
      }
    } catch {}
    await new Promise((r) => setTimeout(r, 1_000));
  }
  throw new Error(
    `Motion Master not reachable at ${mmUrl} after ${timeoutMs}ms — start it first: ./tools/run.sh`,
  );
}

export async function setup() {
  await waitForApi();
}

export async function teardown() {}
