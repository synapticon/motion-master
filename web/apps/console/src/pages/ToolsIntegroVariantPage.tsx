import { Fragment, useEffect, useMemo, useState } from 'react'
import {
  apiErrorMessage,
  type IntegroVariant,
  type IntegroVariantOption,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import FilePickerButton from '../components/FilePickerButton'
import IntegroVariantView, { multipleSelections } from '../components/IntegroVariantView'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const cardCls = 'border border-grey-200 bg-white p-5 space-y-4'
const thCls =
  'text-left font-display uppercase text-[10px] tracking-wide text-grey-500 px-3 py-2 border-b border-grey-200 align-bottom'
const tdCls = 'px-3 py-2 border-b border-grey-100 align-top text-sm'

/**
 * The catalogue as one table, grouped by category, with the loaded file's choices marked.
 *
 * Grouped rather than flat because a category is the unit that means something: four rows reading
 * "Fieldbus Protocol" tell you less than one heading over four choices, and the grouping is what
 * makes "this category selects exactly one" visible at a glance — every option in it lists the
 * others as incompatible. Marking selections in the same table rather than repeating them in a
 * second one is the other half: the useful question about an option is usually "did this device get
 * it, and what were the alternatives".
 */
function CatalogueTable({
  options,
  selected,
  alternativeIds,
}: {
  options: IntegroVariantOption[]
  selected: Set<number>
  alternativeIds: Set<number>
}) {
  // Category order follows the catalogue's own code order — the first code in a category places it —
  // so the table reads in the order the option ids were assigned rather than alphabetically.
  const groups = useMemo(() => {
    const byCategory = new Map<string, IntegroVariantOption[]>()
    for (const option of options) {
      const category = option.category ?? 'Not in the catalogue'
      const group = byCategory.get(category)
      if (group) {
        group.push(option)
      } else {
        byCategory.set(category, [option])
      }
    }
    return [...byCategory.entries()]
  }, [options])

  return (
    <div className="overflow-x-auto">
      <table className="w-full border-collapse">
        <thead>
          <tr>
            <th className={`${thCls} whitespace-nowrap`}>Selected</th>
            <th className={`${thCls} whitespace-nowrap`}>ID</th>
            <th className={thCls}>Meaning</th>
            <th className={thCls}>SoC variables</th>
            <th className={`${thCls} whitespace-nowrap`}>Rules out</th>
            <th className={`${thCls} whitespace-nowrap`}>MPN</th>
          </tr>
        </thead>
        <tbody>
          {groups.map(([category, group]) => (
            <Fragment key={category}>
              <tr>
                <th
                  colSpan={6}
                  className="text-left font-display uppercase text-[11px] tracking-wide text-grey-600 bg-grey-50 px-3 py-1.5 border-b border-grey-200"
                >
                  {category}
                </th>
              </tr>
              {group.map(option => {
                const isSelected = selected.has(option.id)
                const hasSelectedAlternative = alternativeIds.has(option.id)
                return (
                  <tr
                    key={option.id}
                    className={isSelected ? 'bg-status-good/5' : undefined}
                    aria-selected={isSelected}
                  >
                    {/* Words rather than a coloured dot with the meaning in a tooltip: "yes" and
                        "one of several" are short enough to say outright. */}
                    <td className={`${tdCls} whitespace-nowrap`}>
                      {isSelected && (
                        <span
                          className={hasSelectedAlternative ? 'text-status-info' : 'text-status-good'}
                        >
                          {hasSelectedAlternative ? 'one of several' : 'yes'}
                        </span>
                      )}
                    </td>
                    <td className={`${tdCls} font-mono`}>{option.id}</td>
                    <td className={`${tdCls} ${isSelected ? 'font-medium' : 'text-grey-600'}`}>
                      {option.meaning ?? 'Not in the catalogue'}
                    </td>
                    {/* The SoC variable lists run to fifteen names for a current limit, so they wrap
                        small rather than forcing the table wider than the viewport. */}
                    <td className={`${tdCls} font-mono text-xs text-grey-500`}>
                      {option.socVariables?.join(', ')}
                    </td>
                    <td className={`${tdCls} font-mono text-xs text-grey-500`}>
                      {option.incompatibleOptionIds?.join(', ')}
                    </td>
                    <td className={`${tdCls} font-mono text-xs text-grey-500`}>
                      {option.mpnSegmentCodes?.join(', ')}
                    </td>
                  </tr>
                )
              })}
            </Fragment>
          ))}
        </tbody>
      </table>
    </div>
  )
}

/**
 * Decodes a `.variant` file, and lists the whole option catalogue.
 *
 * Both halves come from the server: the file through the same C++ decoder the device read path uses,
 * and the catalogue from the table that lives beside it. Neither is duplicated here — the catalogue
 * in particular belongs to the firmware, and a second copy in TypeScript would be a second thing to
 * keep in step with a drive.
 */
export default function ToolsIntegroVariantPage() {
  const { api } = useConnection()
  const [filename, setFilename] = useState<string | null>(null)
  const [byteCount, setByteCount] = useState<number | null>(null)
  const [variant, setVariant] = useState<IntegroVariant | null>(null)
  const [parsing, setParsing] = useState(false)
  const [error, setError] = useState<string | null>(null)
  const [catalogue, setCatalogue] = useState<IntegroVariantOption[]>([])
  const [onlySelected, setOnlySelected] = useState(false)

  // The catalogue is static server-side data, so it is fetched once and kept — it is the same table
  // whether or not a file is loaded, and it is what makes the page useful with no file at all.
  useEffect(() => {
    let cancelled = false
    api
      .listIntegroVariantOptions()
      .then(res => {
        if (!cancelled) {
          setCatalogue(res.data)
        }
      })
      .catch(() => {
        // A missing catalogue costs the reference table and nothing else; the file decode below is
        // the page's real job and works without it.
      })
    return () => {
      cancelled = true
    }
  }, [api])

  async function loadFile(file: File) {
    setParsing(true)
    setError(null)
    setVariant(null)
    try {
      const raw = new Uint8Array(await file.arrayBuffer())
      setFilename(file.name)
      setByteCount(raw.length)
      // Posted as raw bytes rather than through the generated client, which JSON-encodes its body.
      const res = await fetch(`${api.baseUrl}/api/integro-variant/parse`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: raw,
      })
      if (!res.ok) {
        const body = (await res.json().catch(() => null)) as { error?: string } | null
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
      setVariant((await res.json()) as IntegroVariant)
    } catch (err) {
      setError(apiErrorMessage(err, 'Failed to parse the variant file.'))
    } finally {
      setParsing(false)
    }
  }

  const selectedIds = useMemo(
    () => new Set((variant?.options ?? []).map(option => option.id)),
    [variant],
  )

  // Which selected options have a selected alternative, so the catalogue table can mark them the
  // way the decoded view above does. The pairing itself belongs to that view.
  const alternativeIds = useMemo(() => {
    const ids = new Set<number>()
    if (variant) {
      for (const selection of multipleSelections(variant)) {
        for (const option of selection.options) {
          ids.add(option.id)
        }
      }
    }
    return ids
  }, [variant])

  // Codes the file carries that the catalogue does not name. Shown as their own rows so a file from
  // a future firmware still lists everything it selects rather than silently dropping it.
  const unknownOptions = useMemo(
    () => (variant?.options ?? []).filter(option => option.category === undefined),
    [variant],
  )

  const tableOptions = useMemo(() => {
    const rows = onlySelected
      ? catalogue.filter(option => selectedIds.has(option.id))
      : [...catalogue]
    return [...rows, ...unknownOptions]
  }, [catalogue, onlySelected, selectedIds, unknownOptions])

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="Integro Variant"
        description="Load a .variant file and decode which features a SOMANET Integro was licensed with — no device required. To read one off a connected device instead, use its Files page."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <section className={cardCls}>
          <div>
            <h3 className="font-display uppercase text-sm tracking-wide">Variant file</h3>
            <p className="text-xs text-grey-500 mt-1">
              Only Integro drives carry a <span className="font-mono">.variant</span>. Its layout
              comes from the firmware that reads it rather than from a specification, and the one
              thing on it that matters beyond inspection is the fieldbus protocol: the hardware
              description deliberately does not carry it, and a full firmware descriptor ends with
              it.
            </p>
          </div>

          <div className="flex items-center gap-3">
            {/* No `accept`: the file is named `.variant`, an extension no dialog filter matches. */}
            <FilePickerButton accept="" onFile={file => void loadFile(file)} disabled={parsing}>
              {variant ? 'Load another file' : 'Load variant file'}
            </FilePickerButton>
            {filename && (
              <span className="text-xs text-grey-500 font-mono">
                {filename}
                {byteCount !== null && ` · ${byteCount} bytes`}
              </span>
            )}
          </div>

          {parsing && <p className="text-xs text-grey-600">Parsing…</p>}
          {error && <Callout variant="error">{error}</Callout>}
        </section>

        {variant && (
          <section className={cardCls}>
            <h3 className="font-display uppercase text-sm tracking-wide">Decoded</h3>
            <IntegroVariantView variant={variant} />
          </section>
        )}

        {catalogue.length > 0 && (
          <section className={cardCls}>
            <div className="flex items-start justify-between gap-4">
              <div>
                <h3 className="font-display uppercase text-sm tracking-wide">
                  {variant ? 'Options, with this file marked' : 'All Integro variant options'}
                </h3>
                <p className="text-xs text-grey-500 mt-1">
                  Every option an Integro can be licensed with, grouped by what it selects. The codes
                  ascend with real gaps, and some are no longer implemented by current firmware —
                  they stay in the catalogue because files written years ago still carry them.
                </p>
              </div>
              {variant && (
                <label className="flex items-center gap-2 text-xs text-grey-600 whitespace-nowrap">
                  <input
                    type="checkbox"
                    checked={onlySelected}
                    onChange={e => setOnlySelected(e.target.checked)}
                  />
                  Only selected
                </label>
              )}
            </div>
            <CatalogueTable
              options={tableOptions}
              selected={selectedIds}
              alternativeIds={alternativeIds}
            />
          </section>
        )}
      </div>
    </div>
  )
}
