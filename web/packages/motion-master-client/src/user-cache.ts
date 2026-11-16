/**
 * Percent-encodes each folder and the name of a path in the user cache. The `/` between them stays,
 * because the server reads a nested path from the segments. The helpers of the user cache encode
 * the path themselves. A call of `client.api`, such as `getUserCacheFile`, takes the encoded path.
 */
export function encodeUserCachePath(path: string): string {
  return path.split('/').map(encodeURIComponent).join('/');
}
