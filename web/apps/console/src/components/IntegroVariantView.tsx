import { useMemo } from 'react'
import type { IntegroVariant, IntegroVariantFileOption } from '@synapticon/motion-master-client'
import Callout from './Callout'
import FieldRow from './FieldRow'

/** Several options selected where the file should carry one. */
export interface MultipleSelection {
  category: string
  options: IntegroVariantFileOption[]
}

/**
 * The categories where this file selects more than one option.
 *
 * A production device has one option selected per category, so a group here means a development
 * device — one unit set up to be tested in more than one configuration. Exported because the Tools
 * page marks the same options in its catalogue table.
 *
 * Grouped by the alternatives each option lists rather than by category name, because one category
 * name covers two shapes: "Digital IOs allowed" holds both the one-of-four bitmap options and the
 * per-line disable flags, and selecting several of the latter is ordinary.
 */
export function multipleSelections(variant: IntegroVariant): MultipleSelection[] {
  const byId = new Map(variant.options.map(option => [option.id, option]))
  const grouped = new Set<number>()
  const selections: MultipleSelection[] = []

  for (const option of variant.options) {
    if (grouped.has(option.id)) {
      continue
    }
    // Everything reachable from this option through the alternatives relation, so three mutually
    // alternative options come out as one group of three rather than three pairs.
    const group: IntegroVariantFileOption[] = []
    const queue = [option]
    grouped.add(option.id)
    while (queue.length > 0) {
      const current = queue.shift() as IntegroVariantFileOption
      group.push(current)
      for (const id of current.incompatibleOptionIds ?? []) {
        const alternative = byId.get(id)
        if (alternative && !grouped.has(id)) {
          grouped.add(id)
          queue.push(alternative)
        }
      }
    }
    if (group.length > 1) {
      group.sort((a, b) => a.id - b.id)
      selections.push({ category: group[0].category ?? 'Not in the catalogue', options: group })
    }
  }
  return selections
}

/**
 * A decoded `.variant`: the header, and which options the file selects.
 *
 * Shared by the Integro Variant tool, which parses a file from disk and adds the whole option
 * catalogue beside this, and the Files page, which reads one off a device.
 */
export default function IntegroVariantView({ variant }: { variant: IntegroVariant }) {
  const allSelections = useMemo(() => multipleSelections(variant), [variant])

  const fieldbus = variant.fieldbusProtocol
  const fieldbusOption = variant.options.find(option => option.id === fieldbus)

  // The other protocols the file selects, if any. Only one is ever in effect, so they belong on the
  // fieldbus row as context rather than in the note below — which then has nothing to say about a
  // file whose only multiple selection is the fieldbus.
  const otherFieldbuses = useMemo(() => {
    const group = allSelections.find(selection =>
      selection.options.some(option => option.id === fieldbus),
    )
    return (group?.options ?? []).filter(option => option.id !== fieldbus)
  }, [allSelections, fieldbus])

  const selections = useMemo(
    () =>
      allSelections.filter(
        selection => !selection.options.some(option => option.id === fieldbus),
      ),
    [allSelections, fieldbus],
  )

  // The part-number segments the selection implies — a compact way to check a file against the
  // hardware it was issued for.
  const mpnSegments = variant.options.flatMap(option => option.mpnSegmentCodes ?? [])

  return (
    <div className="space-y-4">
      <div>
        <FieldRow label="File version" value={String(variant.fileVersion)} />
        <FieldRow label="Serial number" value={variant.serialNumber} />
        <FieldRow label="MAC address" value={variant.macAddress} />
        <FieldRow label="netX chip ID" value={variant.chipId} />
        <FieldRow label="Customer ID" value={String(variant.customerId)} />
        <FieldRow
          label="Operation mode"
          value={variant.operationModeName}
          hint={
            variant.operationModeName === 'passive'
              ? 'the drive falls back to this when the file is missing, unsigned, or issued for another chip'
              : `0x${variant.operationMode.toString(16).toUpperCase()}`
          }
        />
        <FieldRow
          label="Fieldbus protocol"
          value={fieldbus === undefined ? 'none selected' : String(fieldbus)}
          hint={
            otherFieldbuses.length > 0
              ? `${fieldbusOption?.meaning ?? ''} — in effect. This file also selects ${otherFieldbuses
                  .map(option => option.meaning)
                  .join(' and ')}, which a development device carries so one unit can be tested on more than one protocol.`
              : fieldbusOption?.meaning
          }
        />
        {mpnSegments.length > 0 && (
          <FieldRow
            label="MPN segments"
            value={mpnSegments.join(' · ')}
            hint="part-number segments this selection implies"
          />
        )}
        <FieldRow label="Signature" value={variant.signature} hint="verified by the drive" />
      </div>

      {selections.length > 0 && (
        <Callout variant="info">
          <p>
            This looks like a development device. A production device has one option selected per
            category, and this file selects several — which is how a single unit is set up to be
            tested in more than one configuration.
          </p>
          <ul className="list-disc pl-4 mt-1">
            {selections.map(selection => (
              <li key={selection.category}>
                {selection.category}:{' '}
                {selection.options.map((option, index) => (
                  <span key={option.id}>
                    {index > 0 && ', '}
                    <span className="font-mono">{option.id}</span> {option.meaning}
                  </span>
                ))}
              </li>
            ))}
          </ul>
        </Callout>
      )}

      <div>
        <p className="eyebrow mb-2">Selected, in file order</p>
        {/* Every chip here is selected — that is what the section is — so they are styled alike.
            Tinting the ones sharing a category with another selection made those look like the only
            selected options. */}
        <div className="flex flex-wrap gap-1.5">
          {variant.options.map((option, index) => (
            <span
              key={`${option.id}-${index}`}
              className="inline-flex flex-col gap-0.5 border border-grey-200 bg-grey-50 px-2 py-1.5 text-xs"
            >
              {/* Category above the choice, so the chips stay narrow enough to scan down a column
                  and each one says what it is setting as well as what it is set to. */}
              <span className="text-[10px] uppercase tracking-wide text-grey-500 font-display">
                {option.category ?? 'Not in the catalogue'}
              </span>
              <span className="flex items-baseline gap-1.5">
                <span className="font-mono text-grey-500">{option.id}</span>
                <span>{option.meaning ?? '—'}</span>
              </span>
            </span>
          ))}
        </div>
      </div>
    </div>
  )
}
