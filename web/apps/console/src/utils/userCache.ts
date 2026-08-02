// Helpers for the `/api/user-cache` file store, shared by the Storage → User Cache page (which
// manages every file) and the Recorder page (which only cares about the `dumps/` folder).

/// Folder the server writes `.mmpd` recorder dumps into, relative to the user-cache root.
///
/// The source of truth is `DeviceManager::dumpProcessData` in libs/node/device_manager.cc, which
/// resolves an empty `recorder.dumpDir` to `<user cache root>/dumps`. A deployment that overrides
/// `recorder.dumpDir` to a path outside the cache writes dumps somewhere this store cannot see —
/// callers should treat an empty listing as "none here", never as "none exist".
export const DUMPS_FOLDER = 'dumps'

/// Encodes a cache-relative path for use in a `/api/user-cache/...` URL.
///
/// Each segment is encoded on its own: the backend takes the path after the prefix verbatim (then
/// percent-decodes it), so encoding the whole string would turn its `/` separators into `%2F` and
/// the server would see one long filename instead of a nested path.
export function encodeUserCachePath(path: string): string {
  return path.split('/').map(encodeURIComponent).join('/')
}

/// Absolute URL of one file in the user cache.
export function userCacheUrl(baseUrl: string, path: string): string {
  return `${baseUrl}/api/user-cache/${encodeUserCachePath(path)}`
}

/// The file's own name, without its folders — what a browser download should be called (a download
/// cannot create directories, and a `/` in the suggested name is stripped anyway).
export function userCacheBasename(path: string): string {
  return path.split('/').pop() ?? path
}
