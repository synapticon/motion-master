// Helpers for the `/api/user-cache` file store, shared by the Storage → User Cache page (which
// manages every file) and the Recorder page (which only cares about the `dumps/` folder).

/**
 * Folder the server writes `.mmpd` recorder dumps into, relative to the user-cache root.
 *
 * The source of truth is `DeviceManager::dumpProcessData` in libs/node/device_manager.cc, which
 * resolves an empty `recorder.dumpDir` to `<user cache root>/dumps`. A deployment that overrides
 * `recorder.dumpDir` to a path outside the cache writes dumps somewhere this store cannot see —
 * callers should treat an empty listing as "none here", never as "none exist".
 */
export const DUMPS_FOLDER = 'dumps';

/**
 * Folder the server finds a device's ESI in, relative to the user-cache root. A file is named
 * `esi-<vendor>-<version>.xml`; the source of truth is `EsiCache` in libs/node/esi_cache.h.
 */
export const ESIS_FOLDER = 'esis';

/**
 * The file's own name, without its folders — what a browser download should be called (a download
 * cannot create directories, and a `/` in the suggested name is stripped anyway).
 */
export function userCacheBasename(path: string): string {
  return path.split('/').pop() ?? path;
}
