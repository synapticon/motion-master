export async function logFetch(
  tag: string,
  input: Parameters<typeof fetch>[0],
  init?: Parameters<typeof fetch>[1],
): Promise<Response> {
  const method = init?.method ?? 'GET';
  const url = typeof input === 'string' ? input : input instanceof URL ? input.toString() : input.url;
  const start = Date.now();
  try {
    const res = await fetch(input, init);
    console.log(`[${tag}] ${method} ${url} → ${res.status} (${Date.now() - start}ms)`);
    return res;
  } catch (e) {
    const msg = e instanceof Error ? e.message : String(e);
    console.log(`[${tag}] ${method} ${url} → ERROR (${Date.now() - start}ms): ${msg}`);
    throw e;
  }
}
