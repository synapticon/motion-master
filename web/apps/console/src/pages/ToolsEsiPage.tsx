import { Fragment, useMemo, useState } from 'react'
import type { EsiDeviceSummary, EsiEntry, EsiParseResult } from '@synapticon/motion-master-client'
import { ChevronRight } from 'lucide-react'
import Checkbox, { selectAllState } from '../components/Checkbox'
import EsiExplainer from '../components/EsiExplainer'
import FilePickerButton from '../components/FilePickerButton'
import PageHeader from '../components/PageHeader'
import { WireTiming, useWireTiming } from '../components/WireTiming'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const inputCls = 'h-[38px] border border-grey-300 px-3 text-sm w-full bg-white'

function toHex(value: number, digits: number): string {
  return `0x${value.toString(16).toUpperCase().padStart(digits, '0')}`
}

// An entry's address, the way the CoE world writes it: 0x6040:00.
function address(entry: EsiEntry): string {
  return `${toHex(entry.index, 4)}:${entry.subindex.toString(16).toUpperCase().padStart(2, '0')}`
}

// Access as a config tool spells it: the mode, plus any AL-state restriction the ESI attaches.
function formatAccess(entry: EsiEntry): string {
  const parts: string[] = [entry.access.mode]
  if (entry.access.readRestrictions) {
    parts.push(`r:${entry.access.readRestrictions}`)
  }
  if (entry.access.writeRestrictions) {
    parts.push(`w:${entry.access.writeRestrictions}`)
  }
  return parts.join(' ')
}

// Descriptions in an ESI are XML-escaped HTML written by the vendor. Rendering that markup would
// mean trusting an uploaded file with dangerouslySetInnerHTML, so it is reduced to readable plain
// text instead: block tags become breaks, the rest are dropped, and the handful of entities that
// survive XML parsing (the file escapes them twice) are decoded.
function toPlainText(html: string): string {
  return html
    .replace(/<\/(p|div|li|tr|h[1-6])>/gi, '\n')
    .replace(/<br\s*\/?>/gi, '\n')
    .replace(/<li[^>]*>/gi, '• ')
    .replace(/<[^>]+>/g, '')
    .replace(/&nbsp;/g, ' ')
    .replace(/&lt;/g, '<')
    .replace(/&gt;/g, '>')
    .replace(/&quot;/g, '"')
    .replace(/&#39;/g, "'")
    .replace(/&amp;/g, '&')
    .replace(/[ \t]+\n/g, '\n')
    .replace(/\n{3,}/g, '\n\n')
    .trim()
}

// A hexBinary default can be the whole width of its type — a STRING(50) is 100 characters — which
// would set the column width for every other row. Show a readable head and hand the rest to the
// tooltip; the full value is also in the expanded row.
const kMaxDataChars = 16

function DataCell({ hex }: { hex?: string }) {
  if (!hex) {
    return <span className="text-grey-400">—</span>
  }
  if (hex.length <= kMaxDataChars) {
    return <>{hex}</>
  }
  return (
    <span title={hex} className="cursor-help">
      {hex.slice(0, kMaxDataChars)}…
    </span>
  )
}

// The module's name is the longest thing in the row and the same on hundreds of consecutive rows,
// so the cell carries the ident and slot — the parts that actually distinguish one source from
// another — and the name goes to the tooltip.
function SourceCell({ entry, moduleNames }: { entry: EsiEntry; moduleNames: Map<number, string> }) {
  if (entry.source.kind === 'device') {
    return (
      <span title="The device's own dictionary, not a module" className="cursor-help">
        device
      </span>
    )
  }
  const ident = entry.source.moduleIdent ?? 0
  const slot = entry.source.slot ?? 0
  const name = moduleNames.get(ident)
  return (
    <span
      title={`${name ? `${name} — ` : ''}module ${toHex(ident, 8)}, slot ${slot}`}
      className="cursor-help"
    >
      {toHex(ident, 8)} · {slot}
    </span>
  )
}

interface DeviceDictionaryProps {
  device: EsiDeviceSummary
  /** ModuleIdent -> module name, so an entry's source reads as a name rather than a number. The
      name is not repeated on every entry; it is looked up here from the file's module list. */
  moduleNames: Map<number, string>
}

// One device's assembled dictionary: a filterable table with expandable rows. Rendered per device
// because the endpoint returns every device's entries in one response — there is no selector.
function DeviceDictionary({ device, moduleNames }: DeviceDictionaryProps) {
  const [filter, setFilter] = useState('')
  const [expanded, setExpanded] = useState<string | null>(null)
  const [open, setOpen] = useState(false)

  const entries = useMemo(() => device.entries ?? [], [device.entries])
  const filtered = useMemo(() => {
    const needle = filter.trim().toLowerCase()
    if (!needle) {
      return entries
    }
    return entries.filter(e =>
      [address(e), e.objectName, e.entryName, e.displayName, e.dataTypeName, e.description ?? '']
        .join(' ')
        .toLowerCase()
        .includes(needle),
    )
  }, [entries, filter])

  return (
    <section className="border border-grey-200">
      <button
        type="button"
        onClick={() => setOpen(o => !o)}
        aria-expanded={open}
        className="w-full flex items-center justify-between gap-3 px-4 py-3 text-left hover:bg-grey-50 transition-colors cursor-pointer"
      >
        <span>
          <span className="font-display uppercase tracking-wide text-sm text-grey-800">
            {device.type}
          </span>
          {/* normal-case opts out of the design system's global uppercase for buttons
              (theme.css). That rule is right for a button's label — the device type beside this
              keeps it — but this span is data: uppercased it renders 0X00000201 and "506 ENTRIES",
              contradicting the same values shown as 0x00000201 in the tables below. */}
          <span className="ml-3 text-xs text-grey-500 font-mono normal-case">
            {device.productCode !== undefined && toHex(device.productCode, 8)}
            {device.revisionNo !== undefined && ` rev ${toHex(device.revisionNo, 8)}`}
            {` · ${entries.length} entries`}
            {device.warnings && device.warnings.length > 0 && ` · ${device.warnings.length} warnings`}
          </span>
        </span>
        <span className="text-xs text-grey-500">{open ? 'Hide' : 'Show'}</span>
      </button>

      {open && (
        <div className="border-t border-grey-200 px-4 py-4">
          <div className="flex flex-wrap items-center gap-3 mb-3">
            <div className="relative max-w-sm w-full">
              <input
                type="text"
                value={filter}
                onChange={e => setFilter(e.target.value)}
                placeholder="Filter by address, name, or type…"
                className={`${inputCls} pr-8`}
              />
              {filter && (
                <button
                  type="button"
                  onClick={() => setFilter('')}
                  className="absolute right-2 top-1/2 -translate-y-1/2 text-grey-400 hover:text-syn-red cursor-pointer leading-none text-lg"
                  title="Clear filter"
                  aria-label="Clear filter"
                >
                  ×
                </button>
              )}
            </div>
            <span className="text-xs text-grey-500 whitespace-nowrap">
              {filtered.length === entries.length
                ? `${entries.length} entries`
                : `${filtered.length} / ${entries.length}`}
            </span>
          </div>

          {entries.length === 0 ? (
            <p className="text-xs text-grey-500">This device declares no object dictionary.</p>
          ) : (
            <div className="border border-grey-200 overflow-x-auto">
              <table className="w-full text-xs border-collapse">
                <thead>
                  <tr className="border-b border-grey-200 bg-grey-50">
                    <th className="w-px" />
                    {[
                      'Address',
                      'Object',
                      'Entry',
                      'Kind',
                      'Type',
                      'Bits',
                      'Access',
                      'Cat',
                      'PDO',
                      'Default',
                      'Min',
                      'Max',
                      'Unit',
                      'Source',
                    ].map(h => (
                      <th
                        key={h}
                        className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                      >
                        {h}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {filtered.map(e => {
                    const key = `${e.index}:${e.subindex}`
                    const detail = toPlainText(e.description ?? '')
                    // A value the table had to truncate is itself a reason to expand: a tooltip
                    // cannot be selected or copied, and this is a hex value someone will want to
                    // paste somewhere.
                    const truncated = [e.defaultData, e.minData, e.maxData].some(
                      v => (v?.length ?? 0) > kMaxDataChars,
                    )
                    const isOpen = expanded === key
                    return [
                      <tr
                        key={key}
                        onClick={() => setExpanded(isOpen ? null : key)}
                        className={`border-b border-grey-100 cursor-pointer hover:bg-grey-50 ${
                          isOpen ? 'bg-grey-50' : ''
                        }`}
                      >
                        <td className="pl-3 py-1.5 w-px">
                          <ChevronRight
                            aria-hidden="true"
                            className={`w-3.5 h-3.5 text-grey-400 transition-transform ${
                              isOpen ? 'rotate-90' : ''
                            }`}
                          />
                        </td>
                        <td className="px-3 py-1.5 font-mono whitespace-nowrap">{address(e)}</td>
                        <td className="px-3 py-1.5 text-grey-800">{e.objectName}</td>
                        <td className="px-3 py-1.5 text-grey-700">
                          {e.displayName !== e.entryName ? (
                            <span title={`ESI display name; the SubItem is named "${e.entryName}"`}>
                              {e.displayName}
                            </span>
                          ) : (
                            e.entryName
                          )}
                        </td>
                        <td className="px-3 py-1.5 font-mono text-grey-500">{e.objectCode}</td>
                        <td className="px-3 py-1.5 font-mono text-grey-700">{e.dataTypeName}</td>
                        <td className="px-3 py-1.5 font-mono text-grey-700">{e.bitSize}</td>
                        <td className="px-3 py-1.5 font-mono text-grey-700 whitespace-nowrap">
                          {formatAccess(e)}
                        </td>
                        <td className="px-3 py-1.5 font-mono text-grey-500">{e.category}</td>
                        <td className="px-3 py-1.5 font-mono text-grey-500">{e.pdoMapping || '—'}</td>
                        <td className="px-3 py-1.5 font-mono whitespace-nowrap">
                          <DataCell hex={e.defaultData} />
                        </td>
                        <td className="px-3 py-1.5 font-mono whitespace-nowrap">
                          <DataCell hex={e.minData} />
                        </td>
                        <td className="px-3 py-1.5 font-mono whitespace-nowrap">
                          <DataCell hex={e.maxData} />
                        </td>
                        <td className="px-3 py-1.5 font-mono">{e.unitSymbol || '—'}</td>
                        <td className="px-3 py-1.5 font-mono text-grey-500 whitespace-nowrap">
                          <SourceCell entry={e} moduleNames={moduleNames} />
                        </td>
                      </tr>,
                      isOpen ? (
                        <tr key={`${key}-detail`} className="border-b border-grey-100 bg-grey-50">
                          <td colSpan={15} className="px-4 py-3 space-y-3">
                            {detail && (
                              <p className="text-xs text-grey-700 whitespace-pre-line max-w-4xl leading-relaxed">
                                {detail}
                              </p>
                            )}
                            {!detail &&
                              (e.subindex !== 0 ? (
                                // Object-level text lives on subindex 0 rather than being copied
                                // onto every member, so point there instead of leaving a blank.
                                <p className="text-xs text-grey-500">
                                  No description of its own — see{' '}
                                  <span className="font-mono">{toHex(e.index, 4)}:00</span> for the
                                  object's.
                                </p>
                              ) : (
                                // Every row expands, so one with nothing but the metadata below
                                // needs to say the ESI is silent rather than look half-rendered.
                                <p className="text-xs text-grey-500">
                                  This ESI gives no description for this object.
                                </p>
                              ))}
                            {truncated && (
                              <div>
                                <p className="eyebrow mb-1">Values</p>
                                <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-0.5 text-xs font-mono text-grey-700">
                                  {([
                                    ['Default', e.defaultData],
                                    ['Min', e.minData],
                                    ['Max', e.maxData],
                                  ] as const).map(([label, hex]) =>
                                    hex ? (
                                      <Fragment key={label}>
                                        <dt className="text-grey-500">{label}</dt>
                                        <dd className="break-all">{hex}</dd>
                                      </Fragment>
                                    ) : null,
                                  )}
                                </dl>
                              </div>
                            )}
                            {(e.options?.length ?? 0) > 0 && (
                              <div>
                                <p className="eyebrow mb-1">Options</p>
                                <ul className="text-xs font-mono text-grey-700 space-y-0.5">
                                  {e.options?.map(o => (
                                    <li key={`${o.label}-${o.value}`}>
                                      {o.value} — {o.label}
                                    </li>
                                  ))}
                                </ul>
                              </div>
                            )}
                            {(e.properties?.length ?? 0) > 0 && (
                              <div>
                                <p className="eyebrow mb-1">Properties</p>
                                <ul className="text-xs font-mono text-grey-600 space-y-0.5">
                                  {e.properties?.map(p => (
                                    <li key={p.name}>
                                      {p.name}: {p.value}
                                    </li>
                                  ))}
                                </ul>
                              </div>
                            )}
                            <p className="text-[11px] text-grey-500 font-mono">
                              ETG.1020 type {toHex(e.dataType, 4)} · bit offset {e.bitOffset} ·
                              ObjAccess {toHex(e.objAccess, 4)}
                              {e.rawIndex !== undefined && ` · declared at ${toHex(e.rawIndex, 4)}`}
                            </p>
                          </td>
                        </tr>
                      ) : null,
                    ]
                  })}
                </tbody>
              </table>
            </div>
          )}

          {device.warnings && device.warnings.length > 0 && (
            <div className="border border-status-warn/40 bg-status-warn/5 mt-4">
              <div className="px-4 py-3 border-b border-status-warn/40">
                <span className="eyebrow">Dictionary warnings</span>
              </div>
              <p className="px-4 pt-3 text-xs text-grey-600">
                Problems found while assembling this device — a value whose length disagrees with
                its declared type, an object referencing an undeclared type, or two modules
                claiming the same address. A different module selection would produce a different
                list.
              </p>
              <ul className="px-4 py-3 space-y-1 text-xs font-mono text-grey-700">
                {device.warnings.map(w => (
                  <li key={w}>{w}</li>
                ))}
              </ul>
            </div>
          )}
        </div>
      )}
    </section>
  )
}

export default function ToolsEsiPage() {
  const { api } = useConnection()
  const [file, setFile] = useState<File | null>(null)
  const [result, setResult] = useState<EsiParseResult | null>(null)
  // Which modules the user has ticked, and which selection the loaded `result` was built with.
  // Keeping both lets Apply enable only when they differ, so an accidental click cannot cost a
  // re-upload of a two-megabyte file.
  const [selected, setSelected] = useState<Set<number>>(new Set())
  const [applied, setApplied] = useState<Set<number>>(new Set())
  const [busy, setBusy] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const { timing, measure, reset: resetTiming } = useWireTiming()

  // The endpoint is stateless — nothing is stored server-side — so re-applying a module selection
  // re-uploads the file. Same trade the SII tool makes, and it keeps a parsed document from
  // outliving the page that asked for it.
  async function parse(f: File, idents: Set<number>) {
    setBusy(true)
    setError(null)
    try {
      // No selection means "merge everything each device's slots reference", which is the
      // endpoint's own default — so send no query at all rather than an empty one.
      const query =
        idents.size > 0 ? `?modules=${encodeURIComponent([...idents].join(','))}` : ''
      const payload = await f.arrayBuffer()
      const res = await measure(() =>
        fetch(`${api.baseUrl}/api/esi/parse${query}`, {
          method: 'POST',
          headers: { 'Content-Type': 'application/xml' },
          body: payload,
        }),
      )
      if (!res.ok) {
        const body = await res.json().catch(() => null)
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
      setResult((await res.json()) as EsiParseResult)
      setApplied(new Set(idents))
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Failed to parse the ESI file.')
      setResult(null)
    } finally {
      setBusy(false)
    }
  }

  // An entry's source names its module by ident only — the name would be the same string on every
  // one of that module's rows — so resolve it once here.
  const moduleNames = useMemo(
    () => new Map((result?.modules ?? []).map(m => [m.moduleIdent, m.type])),
    [result],
  )

  const selectionChanged =
    selected.size !== applied.size || [...selected].some(i => !applied.has(i))

  const selectAll = selectAllState(selected.size, result?.modules.length ?? 0)

  function toggleModule(ident: number) {
    setSelected(prev => {
      const next = new Set(prev)
      if (!next.delete(ident)) {
        next.add(ident)
      }
      return next
    })
  }

  async function loadFile(f: File) {
    setFile(f)
    setSelected(new Set())
    resetTiming()
    await parse(f, new Set())
  }

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="ESI"
        description="Load a vendor's EtherCAT Slave Information (ESI) XML and inspect its object dictionaries offline — no device required. This is the only place to see object descriptions, enum option labels, engineering units and min/max bounds: a device's CoE dictionary carries none of them."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <EsiExplainer />

        <div className="flex flex-wrap items-center gap-3">
          <FilePickerButton onFile={loadFile} disabled={busy} accept=".xml,application/xml,text/xml">
            {file ? 'Load another file' : 'Load ESI file'}
          </FilePickerButton>
          {file && (
            <span className="text-xs text-grey-500 font-mono">
              {file.name} · {(file.size / 1024).toFixed(0)} KB
            </span>
          )}
          {busy && <span className="text-xs text-grey-600">Parsing…</span>}
          {!busy && (
            <WireTiming
              label="Parse"
              timing={timing}
              title="Parse — server-measured time to read the ESI XML and assemble every device's object dictionary, reported by the backend. This is CPU work, not a device transaction; the round-trip beside it also carries the file upload, which for a megabyte-scale ESI dominates."
            />
          )}
        </div>

        {error && <p className="text-xs text-status-bad font-mono">{error}</p>}

        {!file && !busy && (
          <p className="text-xs text-grey-500">
            No file loaded. Choose a <code>.xml</code> ESI file to decode it.
          </p>
        )}

        {result && (
          <>
            <section className="border border-grey-200">
              <div className="px-4 py-3 border-b border-grey-200 bg-grey-50">
                <span className="eyebrow">File</span>
              </div>
              <dl className="px-4 py-3 grid grid-cols-[auto_1fr] gap-x-6 gap-y-1.5 text-xs">
                <dt className="text-grey-600">Vendor</dt>
                <dd className="font-mono">
                  {result.vendor.name} · {toHex(result.vendor.id, 8)}
                </dd>
                <dt className="text-grey-600">Devices</dt>
                <dd className="font-mono">{result.devices.length}</dd>
                <dt className="text-grey-600">Modules</dt>
                <dd className="font-mono">{result.modules.length}</dd>
              </dl>
            </section>

            {/* The modules the file declares, and the control that decides which of them get
                merged into the dictionaries below. One table rather than a list plus a separate
                "name the modules" box: the names are right here, so tick them. */}
            {result.modules.length > 0 && (
              <section>
                <div className="flex flex-wrap items-end justify-between gap-3 mb-3">
                  <div>
                    <h3 className="font-display uppercase tracking-wide text-sm text-grey-700">
                      Modules
                    </h3>
                    {/* Deliberately not written per selection state: a paragraph that changes
                        length reflows every dictionary below it each time a box is ticked. It
                        states the rule for both states instead. */}
                    <p className="text-xs text-grey-600 mt-1 max-w-3xl">
                      A device's objects come from its own dictionary plus the modules in its slots.
                      Tick the ones you know are fitted — <strong>with none ticked, all of them are
                      merged</strong>. Some slots offer alternatives that define the same objects;
                      there the last one wins, and the device's warnings say where.
                    </p>
                  </div>
                  <div className="flex items-center gap-2">
                    <button
                      type="button"
                      disabled={busy || !file || !selectionChanged}
                      onClick={() => file && parse(file, selected)}
                      title={
                        selectionChanged
                          ? 'Re-assemble every device with this module selection.'
                          : 'The dictionaries below already reflect this selection.'
                      }
                      className={`${btnOutline} h-[38px] inline-flex items-center justify-center`}
                    >
                      {busy ? 'Applying…' : 'Apply'}
                    </button>
                  </div>
                </div>

                <div className="border border-grey-200 overflow-x-auto">
                  <table className="w-full text-xs border-collapse">
                    <thead>
                      <tr className="border-b border-grey-200 bg-grey-50">
                        <th className="text-left px-3 py-2 w-px">
                          <Checkbox
                            checked={selectAll.checked}
                            indeterminate={selectAll.indeterminate}
                            onChange={() =>
                              setSelected(
                                selectAll.selectsAll
                                  ? new Set(result.modules.map(m => m.moduleIdent))
                                  : new Set(),
                              )
                            }
                            title={selectAll.selectsAll ? 'Tick every module' : 'Untick every module'}
                            aria-label="Tick every module"
                          />
                        </th>
                        {['Module Ident', 'Type', 'Name', 'Objects', 'Used by'].map(h => (
                          <th
                            key={h}
                            className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap"
                          >
                            {h}
                          </th>
                        ))}
                      </tr>
                    </thead>
                    <tbody>
                      {result.modules.map(m => {
                        // Naming which devices can take a module is what makes the choice
                        // meaningful — otherwise ticking one is a guess about what it affects.
                        const users = result.devices
                          .filter(d => d.moduleIdents.includes(m.moduleIdent))
                          .map(d => d.type)
                        return (
                          <tr
                            key={m.moduleIdent}
                            onClick={() => toggleModule(m.moduleIdent)}
                            className={`border-b border-grey-100 last:border-0 cursor-pointer hover:bg-grey-50 ${
                              selected.has(m.moduleIdent) ? 'bg-syn-red/5' : ''
                            }`}
                          >
                            {/* The whole row toggles, so a click that lands on the checkbox must
                                stop here — otherwise the checkbox toggles and the row's handler
                                immediately toggles it back, and the box appears dead. Stopping at
                                the cell rather than on the input keeps keyboard focus and Space
                                working normally. */}
                            <td className="px-3 py-1.5" onClick={e => e.stopPropagation()}>
                              <Checkbox
                                checked={selected.has(m.moduleIdent)}
                                onChange={() => toggleModule(m.moduleIdent)}
                                aria-label={`Merge ${m.type}`}
                              />
                            </td>
                            <td className="px-3 py-1.5 font-mono">{toHex(m.moduleIdent, 8)}</td>
                            <td className="px-3 py-1.5 text-grey-800">{m.type}</td>
                            <td className="px-3 py-1.5 text-grey-700">{m.name}</td>
                            <td className="px-3 py-1.5 font-mono">{m.objectCount ?? '—'}</td>
                            <td className="px-3 py-1.5 text-grey-500">
                              {users.length === 0 ? (
                                <span title="No device's slots reference this module, so ticking it changes nothing.">
                                  —
                                </span>
                              ) : (
                                users.join(', ')
                              )}
                            </td>
                          </tr>
                        )
                      })}
                    </tbody>
                  </table>
                </div>
              </section>
            )}

            {/* One collapsible dictionary per device — every device's entries arrive in the same
                response, so there is nothing to fetch when one is opened. */}
            {result.devices.map(d => (
              <DeviceDictionary key={d.ordinal} device={d} moduleNames={moduleNames} />
            ))}

            {result.warnings && result.warnings.length > 0 && (
              <section className="border border-status-warn/40 bg-status-warn/5">
                <div className="px-4 py-3 border-b border-status-warn/40">
                  <span className="eyebrow">File warnings</span>
                </div>
                <p className="px-4 pt-3 text-xs text-grey-600">
                  Recoverable problems in the document itself. The file still parsed; these name
                  what was skipped or could not be read. Per-device assembly problems are reported
                  on the device instead.
                </p>
                <ul className="px-4 py-3 space-y-1 text-xs font-mono text-grey-700">
                  {result.warnings.map(w => (
                    <li key={w}>{w}</li>
                  ))}
                </ul>
              </section>
            )}
          </>
        )}
      </div>
    </div>
  )
}
