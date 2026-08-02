import { Link } from 'react-router'
import Explainer from './Explainer'

// The teaching panel for the Storage → User Cache page. It explains that the store lives on the
// server rather than in this browser, what the two folders Motion Master writes itself contain and
// what deleting from each actually costs, that nothing is ever reaped automatically, and which
// paths the backend refuses. Ties back to Recorder (the source of dumps/) and Parameter Cache (the
// page dedicated to parameters/).
export default function UserCacheExplainer() {
  return (
    <Explainer title="What is the user cache?">
      <p>
        A plain <strong>file store on the machine running Motion Master</strong> — not in this
        browser, and not necessarily this computer. Whatever you put here is written to that
        machine&apos;s own disk and stays there across restarts, so a file only has to be uploaded
        once, and a feature that needs one does not have to grow an upload page of its own.
      </p>

      <p>
        <strong>Uploading a file does not, by itself, make Motion Master do anything with it.</strong>{' '}
        Nothing is validated, converted or acted on at upload time — you choose the paths, and a
        file simply sits where you put it. That is a separate question from whether some{' '}
        <em>feature</em> reads a particular file: several own a folder here and parse their own,
        listed below, and more will over time. The rule of thumb is that a feature knows where to
        look for its own files; dropping something into this store does not volunteer it to anything.
      </p>

      <p>
        Uploading <strong>replaces</strong> whatever is already at that path. Sub-directories are
        implied by the path you give — <code>configs/machine-a.json</code> creates{' '}
        <code>configs/</code> on the way in — and deleting the last file in one removes the empty
        folder again, so there is no create-folder step and no husks left behind.
      </p>

      <p>
        <strong>Two folders are Motion Master&apos;s own</strong> today — it writes them, reads them
        back, and they appear in this list because they sit in the same directory:
      </p>
      <ul className="list-disc pl-5 space-y-1">
        <li>
          <code>parameters/</code> — the cached CoE object dictionaries, one file per device
          identity. Deleting one costs a fresh SDO enumeration on the next scan and nothing else; no
          device is touched. The{' '}
          <Link to="/storage/parameter-cache" className="underline hover:text-ocean">
            Parameter Cache
          </Link>{' '}
          page shows the same files by device instead of by path.
        </li>
        <li>
          <code>dumps/</code> — the <code>.mmpd</code> process-data recordings written by{' '}
          <strong>Dump to disk</strong> on the{' '}
          <Link to="/data/recorder" className="underline hover:text-ocean">
            Recorder
          </Link>{' '}
          page, which can also reopen them straight into its chart. These are the big ones — tens of
          megabytes per drive.
        </li>
      </ul>

      <p>
        <strong>Nothing here is ever deleted for you.</strong> That is the point — the system
        temporary directory would have been the easy home, but the OS reaps it on a timer and a
        capture you meant to keep would quietly vanish. The trade is that clearing out old dumps is
        a job someone has to do, and this page is where it happens.
      </p>

      <p>
        Paths are confined to the cache directory: anything absolute, containing{' '}
        <code>..</code>, or naming a device is refused, so no request can read or write elsewhere on
        the server. Downloads are always saved, never opened in the browser. A file could contain a
        script, and a script opened from Motion Master&apos;s own address would be able to command
        the drives as if you had clicked the buttons yourself — so the browser is told to save it
        and never run it.
      </p>
    </Explainer>
  )
}
