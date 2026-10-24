import Explainer from './Explainer'

// The "What is the parameter cache?" teaching panel for the Data → Parameter Caches page.
export default function ParameterCacheExplainer() {
  return (
    <Explainer title="What is the parameter cache?">
      <p>
        Reading a drive's full <strong>object dictionary</strong> over the CoE SDO-Information
        service is hundreds of mailbox round-trips and takes seconds. The result — the parameter{' '}
        <strong>definitions</strong> (index, name, data type, access, units, and default / min / max
        bounds) — is identical for every device of the same <strong>vendor, product, and
        firmware version</strong>, so Motion Master writes it to a small JSON file on disk and reuses it the
        next time it scans the same hardware. That's why re-scanning a familiar bus no longer
        re-enumerates devices it has already seen.
      </p>
      <p>
        Only the <strong>definitions</strong> are cached — never live parameter <em>values</em>. On
        a cache hit each value is still read from the device, so the cache can never show you a
        stale value as if it were current. A cache that no longer matches the device lists objects
        the device does not have, whose reads fail, and hides objects the device added.
      </p>
      <p>
        Each file is keyed by <code>vendor / product / firmware version</code>. The firmware version
        is object <code>0x100A</code>, read from the device before the cache is used, so a firmware
        update reads the new dictionary on the next scan. A device without <code>0x100A</code> is
        not cached. The key is safe for Synapticon drives, whose object dictionary is fully
        determined by those three values (caching is on for Synapticon by default). For other
        vendors it is opt-in via the configuration file, since a vendor that changes its dictionary
        and keeps its firmware version could otherwise serve mismatched definitions.
      </p>
      <p>
        This page is the manual escape hatch: <strong>download</strong> a file to inspect or share
        it offline, or <strong>delete</strong> one to force a fresh enumeration on the next scan
        (handy after a development build that changed the dictionary and kept its version string).
        Deleting a cache never affects the device
        — only Motion Master's saved copy of its dictionary.
      </p>
    </Explainer>
  )
}
