import { useEffect, useMemo, useState, type ReactNode } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage, type ProcedureSnapshot } from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import Checkbox from '../components/Checkbox'
import DevicePageHeader from '../components/DevicePageHeader'
import FilePickerButton from '../components/FilePickerButton'
import { readAsBase64 } from '../components/ProcedureParameters'
import { useConnection } from '../contexts/ConnectionContext'
import { downloadBytes } from '../utils/download'
import { btnOutline, btnPrimary } from '../utils/styles'

const COMPARE = 'config-file-compare'
const LOAD = 'config-file-load'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

type Outcome = 'loads' | 'unknown-object' | 'not-loadable'

interface CompareRow {
  address: string
  index: number
  subindex: number
  name: string
  first: string | null
  second: string | null
  live: unknown
  outcome: Outcome
  differsBetween: boolean
  differsFromLive: boolean
}

interface CompareSide {
  source: 'file' | 'drive'
  rows: number
  firmwareVersion: string | null
  problems: { line: number; message: string }[]
}

interface CompareResult {
  first: CompareSide
  second: CompareSide | null
  driveFirmwareVersion: string | null
  rows: CompareRow[]
  counts: {
    rows: number
    unknownObject: number
    notLoadable: number
    differsBetween: number
    differsFromLive: number
    onlyInFirst: number
    onlyInSecond: number
  }
}

interface PickedFile {
  name: string
  base64: string
}

// What the drive does with a row on a load, in words a reader of the table needs.
const OUTCOME_TEXT: Record<Outcome, string> = {
  loads: 'loads',
  'unknown-object': 'no such object',
  'not-loadable': 'not loadable',
}

const DRIVE_FILE = "the drive's config.csv"

const hex = (value: number, digits: number) =>
  `0x${value.toString(16).toUpperCase().padStart(digits, '0')}`

function stepValue<T>(snapshot: ProcedureSnapshot | undefined, id: string): T | undefined {
  return snapshot?.steps.find(step => step.id === id)?.value as T | undefined
}

function failure(snapshot: ProcedureSnapshot | undefined): string | undefined {
  if (snapshot?.status !== 'failed' && snapshot?.status !== 'cancelled') {
    return undefined
  }
  return snapshot.error ?? snapshot.steps.find(step => step.error)?.error ?? snapshot.status
}

function valueText(value: unknown): string {
  if (value === null || value === undefined) {
    return '—'
  }
  return typeof value === 'string' ? value : JSON.stringify(value)
}

export default function DeviceConfigurationPage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const [showFile, setShowFile] = useState(false)
  const [withValues, setWithValues] = useState(true)
  const [onlyDiffering, setOnlyDiffering] = useState(false)
  const [first, setFirst] = useState<PickedFile | null>(null)
  const [second, setSecond] = useState<PickedFile | null>(null)
  const [view, setView] = useState<'differences' | 'all'>('differences')
  const [onlyChanged, setOnlyChanged] = useState(false)
  const [outcome, setOutcome] = useState<Outcome | 'all'>('all')
  const [selected, setSelected] = useState<Set<string>>(new Set())
  const [strategy, setStrategy] = useState('replace')

  // The server keeps the last run of each procedure, and the page shows it, polled while running.
  const snapshotQuery = (name: string) => ({
    queryKey: ['procedure', slavePosition, name],
    queryFn: () => api.getProcedure(slavePosition, name).then(r => r.data),
    refetchInterval: (query: { state: { data?: ProcedureSnapshot } }) =>
      query.state.data?.status === 'running' ? 500 : false,
    refetchIntervalInBackground: true,
    retry: false,
  })
  const compareQuery = useQuery(snapshotQuery(COMPARE))
  const loadQuery = useQuery(snapshotQuery(LOAD))
  const storeQuery = useQuery({
    ...snapshotQuery('store-parameters'),
    refetchInterval: false as const,
  })
  const proceduresQuery = useQuery({
    queryKey: ['procedures', slavePosition],
    queryFn: () => api.listProcedures(slavePosition).then(r => r.data),
    retry: false,
  })
  // The drive's config.csv as rows with names, and with the values in memory when asked. Read only
  // while it is shown: a read is an FoE transfer, and the values one SDO read each.
  const configFileQuery = useQuery({
    queryKey: ['config-file', slavePosition, withValues],
    queryFn: () =>
      api.getDeviceConfigFile(slavePosition, { live: withValues }).then(r => r.data),
    enabled: showFile,
    retry: false,
  })
  const descriptor = (name: string) =>
    proceduresQuery.data?.find(listing => listing.descriptor.name === name)?.descriptor
  const strategyParameter = descriptor(LOAD)?.parameters.find(p => p.name === 'strategy')

  const start = (name: string, body: Record<string, unknown>) =>
    api.startProcedure(slavePosition, name, body).then(r => {
      void queryClient.invalidateQueries({ queryKey: ['procedure', slavePosition, name] })
      return r.data
    })
  const compare = useMutation({
    mutationFn: ({ a, b }: { a: PickedFile; b: PickedFile | null }) =>
      start(COMPARE, { file: a.base64, ...(b ? { other: b.base64 } : {}) }),
  })
  const load = useMutation({
    mutationFn: ({ picked, rows }: { picked: PickedFile; rows: string[] }) =>
      start(LOAD, { file: picked.base64, strategy, rows }),
  })
  const download = useMutation({
    mutationFn: async () => {
      const response = await fetch(`${api.baseUrl}/api/devices/${slavePosition}/files/config.csv`)
      if (!response.ok) {
        const json = await response.json().catch(() => null)
        throw new Error(json?.error ?? `HTTP ${response.status}`)
      }
      downloadBytes(new Uint8Array(await response.arrayBuffer()), `config-${slavePosition}.csv`)
    },
  })

  // A load writes config.csv and changes the values in memory, and a store from the drive bar
  // writes config.csv, so the table is read again after either, and a compare runs again.
  const loadFinishedAt = loadQuery.data?.finishedAt
  useEffect(() => {
    if (loadFinishedAt && loadQuery.data?.status === 'succeeded' && first) {
      compare.mutate({ a: first, b: second })
    }
    void queryClient.invalidateQueries({ queryKey: ['config-file', slavePosition] })
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [loadFinishedAt])
  const storeFinishedAt = storeQuery.data?.finishedAt
  useEffect(() => {
    void queryClient.invalidateQueries({ queryKey: ['config-file', slavePosition] })
  }, [storeFinishedAt, queryClient, slavePosition])

  // A compare belongs to the files picked on this page. One that ran elsewhere, or before the page
  // opened, is not shown.
  const result = first ? stepValue<CompareResult>(compareQuery.data, 'compare') : undefined

  // A new compare selects every row of the first file that the drive loads. Keyed on the time the
  // compare finished, because a refetch of the same snapshot is a new object.
  const compareFinishedAt = compareQuery.data?.finishedAt
  useEffect(() => {
    if (result) {
      setSelected(
        new Set(
          result.rows
            .filter(row => row.first !== null && row.outcome === 'loads')
            .map(row => row.address),
        ),
      )
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [compareFinishedAt, first])

  const comparing = compare.isPending || compareQuery.data?.status === 'running'
  const loading = load.isPending || loadQuery.data?.status === 'running'
  const busy = comparing || loading

  async function pickFirst(file: File) {
    const picked = { name: file.name, base64: await readAsBase64(file) }
    setFirst(picked)
    compare.mutate({ a: picked, b: second })
  }
  async function pickSecond(file: File) {
    if (!first) {
      return
    }
    const picked = { name: file.name, base64: await readAsBase64(file) }
    setSecond(picked)
    compare.mutate({ a: first, b: picked })
  }

  const loadable = useMemo(() => (result?.rows ?? []).filter(row => row.first !== null), [result])
  const allSelected = loadable.length > 0 && loadable.every(row => selected.has(row.address))
  const someSelected = loadable.some(row => selected.has(row.address))
  const toggle = (address: string, checked: boolean) =>
    setSelected(previous => {
      const next = new Set(previous)
      if (checked) {
        next.add(address)
      } else {
        next.delete(address)
      }
      return next
    })

  const firstName = first?.name ?? ''
  const secondName = second?.name ?? DRIVE_FILE
  const compareError = compare.isError
    ? apiErrorMessage(compare.error)
    : first
      ? failure(compareQuery.data)
      : undefined
  const loadError = load.isError ? apiErrorMessage(load.error) : failure(loadQuery.data)
  const verify = stepValue<{ differing: { address: string }[] }>(loadQuery.data, 'verify')
  const problems = [
    ...(result?.first.problems ?? []).map(p => ({ ...p, file: firstName })),
    ...(result?.second?.problems ?? []).map(p => ({ ...p, file: secondName })),
  ]
  const firstHasProblems = (result?.first.problems.length ?? 0) > 0
  const configRows = (configFileQuery.data?.rows ?? []).filter(
    row => !onlyDiffering || row.differs,
  )
  const configDiffering = (configFileQuery.data?.rows ?? []).filter(row => row.differs).length

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Configuration"
        description="The drive's config.csv against the values in memory, a compare of two configurations, and the load of a configuration file."
      />

      <div className="p-4 sm:px-8 sm:py-7 space-y-6 max-w-6xl">
        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200 flex flex-wrap items-center justify-between gap-3">
            <h3 className="font-display uppercase tracking-wide text-sm">This drive</h3>
            <div className="flex flex-wrap items-center gap-3">
              <button type="button" className={btnOutline} onClick={() => setShowFile(!showFile)}>
                {showFile ? 'Hide config.csv' : 'Show config.csv'}
              </button>
              <button
                type="button"
                className={btnOutline}
                disabled={download.isPending}
                onClick={() => download.mutate()}
              >
                {download.isPending ? 'Downloading…' : 'Download config.csv'}
              </button>
            </div>
          </div>
          <p className="px-5 py-3 text-xs text-grey-600">
            The drive loads config.csv when it starts. A row whose value differs from the value in
            memory is a change not stored yet: Store parameters in the drive bar writes the values
            in memory to config.csv.
          </p>
          {download.isError && (
            <div className="px-5 pb-3">
              <Callout variant="error">{apiErrorMessage(download.error)}</Callout>
            </div>
          )}
          {showFile && (
            <div className="pb-2">
              <div className="flex flex-wrap items-center gap-x-6 gap-y-2 px-5 pb-3 text-xs">
                <label className="flex items-center gap-2">
                  <Checkbox checked={withValues} onChange={setWithValues} />
                  With the values in memory
                </label>
                {withValues && (
                  <label className="flex items-center gap-2">
                    <Checkbox checked={onlyDiffering} onChange={setOnlyDiffering} />
                    Only rows that differ
                  </label>
                )}
                {configFileQuery.data && (
                  <span className="text-grey-600">
                    {configFileQuery.data.rows.length} rows
                    {withValues ? `, ${configDiffering} differ from memory` : ''}
                    {configFileQuery.data.firmwareVersion
                      ? ` · saved by firmware ${configFileQuery.data.firmwareVersion}`
                      : ''}
                  </span>
                )}
              </div>
              {configFileQuery.isFetching && !configFileQuery.data && (
                <p className="px-5 pb-3 text-xs text-grey-600">Reading…</p>
              )}
              {configFileQuery.isError && (
                <div className="px-5 pb-3">
                  <Callout variant="error">{apiErrorMessage(configFileQuery.error)}</Callout>
                </div>
              )}
              {configFileQuery.data && (
                <Table
                  head={['Name', 'Index', 'Subindex', 'config.csv', ...(withValues ? ['In memory'] : [])]}
                  rows={configRows.map(row => ({
                    key: `${row.line}`,
                    highlight: row.differs,
                    cells: [
                      <NameCell name={row.name} outcome={row.outcome as Outcome} />,
                      hex(row.index, 4),
                      hex(row.subindex, 2),
                      row.value,
                      ...(withValues ? [valueText(row.live)] : []),
                    ],
                  }))}
                  empty={onlyDiffering ? 'Every row holds the value in memory.' : 'config.csv has no row.'}
                />
              )}
            </div>
          )}
        </section>

        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200">
            <h3 className="font-display uppercase tracking-wide text-sm">Compare</h3>
          </div>
          <div className="flex flex-wrap items-end gap-6 px-5 py-4">
            <div>
              <span className={labelCls}>First</span>
              <div className="flex flex-wrap items-center gap-3">
                {first && <span className="text-sm font-mono">{first.name}</span>}
                <FilePickerButton accept=".csv,text/csv,text/plain" disabled={busy} onFile={pickFirst}>
                  {first ? 'Another file' : 'Pick a configuration file'}
                </FilePickerButton>
              </div>
            </div>
            {first && (
              <div>
                <span className={labelCls}>Second</span>
                <div className="flex flex-wrap items-center gap-3">
                  <span className="text-sm font-mono">{secondName}</span>
                  <FilePickerButton accept=".csv,text/csv,text/plain" disabled={busy} onFile={pickSecond}>
                    Pick a file
                  </FilePickerButton>
                  {second && (
                    <button
                      type="button"
                      className={btnOutline}
                      disabled={busy}
                      onClick={() => {
                        setSecond(null)
                        compare.mutate({ a: first, b: null })
                      }}
                    >
                      Use {DRIVE_FILE}
                    </button>
                  )}
                </div>
              </div>
            )}
            {first && (
              <button
                type="button"
                className={`${btnOutline} ml-auto`}
                disabled={busy}
                onClick={() => compare.mutate({ a: first, b: second })}
              >
                {comparing ? 'Comparing…' : 'Compare again'}
              </button>
            )}
          </div>
          <p className="px-5 pb-3 text-xs text-grey-600">
            {first
              ? `Compares ${firstName} with ${secondName}, and both with the values in memory.`
              : `Pick a configuration file to compare it with ${DRIVE_FILE}, or with a second file.`}
          </p>

          {compareError && (
            <div className="px-5 pb-3">
              <Callout variant="error">{compareError}</Callout>
            </div>
          )}
          {result?.first.firmwareVersion &&
            result.driveFirmwareVersion &&
            result.first.firmwareVersion !== result.driveFirmwareVersion && (
              <div className="px-5 pb-3">
                <Callout variant="warning">
                  {firstName} was saved by firmware {result.first.firmwareVersion}, and the drive
                  runs {result.driveFirmwareVersion}. A load is allowed; check the rows the other
                  version names differently.
                </Callout>
              </div>
            )}
          {problems.length > 0 && (
            <div className="px-5 pb-3">
              <Callout variant="warning">
                The drive ignores these lines, or reads them differently, so a load refuses the file:
                <ul className="list-disc list-inside mt-1">
                  {problems.map(problem => (
                    <li key={`${problem.file}-${problem.line}`}>
                      {problem.file}, line {problem.line}: {problem.message}
                    </li>
                  ))}
                </ul>
              </Callout>
            </div>
          )}
          {!result && comparing && <p className="px-5 pb-4 text-sm">Comparing…</p>}

          {result && (
            <div className="flex flex-wrap items-end gap-x-6 gap-y-3 px-5 pb-4 text-xs">
              <Count label="Rows" value={result.counts.rows} />
              <Count label="Different" value={result.counts.differsBetween} />
              <Count label="Only in first" value={result.counts.onlyInFirst} />
              <Count label="Only in second" value={result.counts.onlyInSecond} />
              <Count label="Not in memory" value={result.counts.differsFromLive} />
              <Count label="No such object" value={result.counts.unknownObject} />
              <Count label="Not loadable" value={result.counts.notLoadable} />
              <div className="ml-auto">
                <label className={labelCls} htmlFor="configuration-view">
                  View
                </label>
                <select
                  id="configuration-view"
                  className={inputCls}
                  value={view}
                  onChange={event => setView(event.target.value as 'differences' | 'all')}
                >
                  <option value="differences">Differences</option>
                  <option value="all">All rows</option>
                </select>
              </div>
            </div>
          )}

          {result && view === 'differences' && (
            <Differences result={result} firstName={firstName} secondName={secondName} />
          )}

          {result && view === 'all' && (
            <div className="pb-4">
              <div className="flex flex-wrap items-end gap-6 px-5 pb-3 text-xs">
                <label className="flex items-center gap-2">
                  <Checkbox checked={onlyChanged} onChange={setOnlyChanged} />
                  Only rows that differ, between the two or from memory
                </label>
                <div>
                  <label className={labelCls} htmlFor="configuration-outcome">
                    Outcome
                  </label>
                  <select
                    id="configuration-outcome"
                    className={inputCls}
                    value={outcome}
                    onChange={event => setOutcome(event.target.value as Outcome | 'all')}
                  >
                    <option value="all">All</option>
                    <option value="loads">Loads</option>
                    <option value="unknown-object">No such object</option>
                    <option value="not-loadable">Not loadable</option>
                  </select>
                </div>
              </div>
              <Table
                head={[
                  <Checkbox
                    checked={allSelected}
                    indeterminate={someSelected && !allSelected}
                    onChange={checked =>
                      setSelected(new Set(checked ? loadable.map(row => row.address) : []))
                    }
                    aria-label={`Select every row of ${firstName}`}
                  />,
                  'Name',
                  'Index',
                  'Subindex',
                  firstName,
                  secondName,
                  'In memory',
                ]}
                rows={result.rows
                  .filter(
                    row =>
                      (!onlyChanged || row.differsBetween || row.differsFromLive) &&
                      (outcome === 'all' || row.outcome === outcome),
                  )
                  .map(row => ({
                  key: row.address,
                  highlight: row.differsBetween || row.differsFromLive,
                  cells: [
                    row.first !== null ? (
                      <Checkbox
                        checked={selected.has(row.address)}
                        onChange={checked => toggle(row.address, checked)}
                        aria-label={`Load ${row.address}`}
                      />
                    ) : (
                      ''
                    ),
                    <NameCell name={row.name} outcome={row.outcome} />,
                    hex(row.index, 4),
                    hex(row.subindex, 2),
                    valueText(row.first),
                    valueText(row.second),
                    valueText(row.live),
                  ],
                }))}
                empty="No row."
              />
            </div>
          )}
        </section>

        {first && result && (
          <section className="border border-grey-200 bg-white">
            <div className="px-5 py-3 border-b border-grey-200">
              <h3 className="font-display uppercase tracking-wide text-sm">Load {first.name}</h3>
            </div>
            {(descriptor(LOAD)?.caveats.length ?? 0) > 0 && (
              <ul className="px-5 pt-4 space-y-1 text-xs text-grey-700 list-disc list-inside">
                {descriptor(LOAD)?.caveats.map(caveat => <li key={caveat}>{caveat}</li>)}
              </ul>
            )}
            <div className="flex flex-wrap items-end gap-4 px-5 py-4">
              <div>
                <label className={labelCls} htmlFor="configuration-strategy">
                  Strategy
                </label>
                <select
                  id="configuration-strategy"
                  className={inputCls}
                  value={strategy}
                  onChange={event => setStrategy(event.target.value)}
                >
                  {(strategyParameter?.options ?? []).map(option => (
                    <option key={String(option.value)} value={String(option.value)}>
                      {option.title}
                    </option>
                  ))}
                </select>
                {strategyParameter && (
                  <p className="text-xs text-grey-600 mt-1 max-w-xl">{strategyParameter.description}</p>
                )}
              </div>
            </div>
            {loadError && (
              <div className="px-5 pb-4">
                <Callout variant="error">{loadError}</Callout>
              </div>
            )}
            {loadQuery.data?.status === 'succeeded' && verify && (
              <div className="px-5 pb-4">
                <Callout variant={verify.differing.length > 0 ? 'warning' : 'info'}>
                  {verify.differing.length > 0
                    ? `Loaded. ${verify.differing.length} object(s) read back with another value: ${verify.differing.map(d => d.address).join(', ')}. The drive loads .assembly_config after config.csv, so an assembly value can win.`
                    : 'Loaded. Every loaded object read back with the value of the file.'}
                </Callout>
              </div>
            )}
            <div className="flex flex-wrap items-center gap-3 px-5 py-4 border-t border-grey-100">
              <button
                type="button"
                className={btnPrimary}
                disabled={busy || selected.size === 0 || firstHasProblems}
                onClick={() => load.mutate({ picked: first, rows: allSelected ? [] : [...selected] })}
              >
                {loading ? 'Loading…' : 'Load'}
              </button>
              <span className="text-xs text-grey-600">
                {firstHasProblems
                  ? `${first.name} has lines the drive ignores or reads differently, listed above, so the load would refuse it. Correct the file first.`
                  : allSelected
                  ? `Every row of ${first.name}`
                  : `${selected.size} of ${loadable.length} rows of ${first.name}; choose them in the All rows view`}
              </span>
            </div>
          </section>
        )}
      </div>
    </div>
  )
}

function Differences({
  result,
  firstName,
  secondName,
}: {
  result: CompareResult
  firstName: string
  secondName: string
}) {
  const name = (row: CompareRow) => <NameCell name={row.name} outcome={row.outcome} />
  const address = (row: CompareRow) => [hex(row.index, 4), hex(row.subindex, 2)]
  const different = result.rows.filter(row => row.differsBetween)
  const onlyFirst = result.rows.filter(row => row.first !== null && row.second === null)
  const onlySecond = result.rows.filter(row => row.second !== null && row.first === null)
  return (
    <div className="space-y-5 pb-4">
      <Group title="Different values" count={different.length}>
        <Table
          head={['Name', 'Index', 'Subindex', firstName, secondName, 'In memory']}
          rows={different.map(row => ({
            key: row.address,
            cells: [
              name(row),
              ...address(row),
              valueText(row.first),
              valueText(row.second),
              valueText(row.live),
            ],
          }))}
          empty="Every row in both holds the same value."
        />
      </Group>
      <Group title={`Only in ${firstName}`} count={onlyFirst.length}>
        <Table
          head={['Name', 'Index', 'Subindex', 'Value', 'In memory']}
          rows={onlyFirst.map(row => ({
            key: row.address,
            highlight: row.differsFromLive,
            cells: [name(row), ...address(row), valueText(row.first), valueText(row.live)],
          }))}
          empty="No row."
        />
      </Group>
      <Group title={`Only in ${secondName}`} count={onlySecond.length}>
        <Table
          head={['Name', 'Index', 'Subindex', 'Value', 'In memory']}
          rows={onlySecond.map(row => ({
            key: row.address,
            highlight: row.differsFromLive,
            cells: [name(row), ...address(row), valueText(row.second), valueText(row.live)],
          }))}
          empty="No row."
        />
      </Group>
    </div>
  )
}

function Group({ title, count, children }: { title: string; count: number; children: ReactNode }) {
  return (
    <div>
      <h4 className="px-5 pb-2 text-xs uppercase tracking-wide text-grey-700">
        {title} <span className="text-grey-500">({count})</span>
      </h4>
      {children}
    </div>
  )
}

function Count({ label, value }: { label: string; value: number }) {
  return (
    <div>
      <span className={labelCls}>{label}</span>
      {value}
    </div>
  )
}

function NameCell({ name, outcome }: { name: string; outcome: Outcome }) {
  return (
    <span>
      {name || <span className="text-grey-500">—</span>}
      {outcome !== 'loads' && (
        <span className="ml-2 text-status-warn whitespace-nowrap">{OUTCOME_TEXT[outcome]}</span>
      )}
    </span>
  )
}

function Table({
  head,
  rows,
  empty,
}: {
  head: ReactNode[]
  rows: { key: string; cells: ReactNode[]; highlight?: boolean }[]
  empty: string
}) {
  return (
    <div className="overflow-x-auto max-h-[36rem] overflow-y-auto">
      <table className="w-full text-xs">
        <thead className="sticky top-0 bg-white">
          <tr className="border-y border-grey-200 text-left text-grey-600">
            {head.map((cell, index) => (
              <th key={index} className={`px-3 py-2 font-normal ${index === 0 ? 'pl-5' : ''}`}>
                {cell}
              </th>
            ))}
          </tr>
        </thead>
        <tbody>
          {rows.map(row => (
            <tr
              key={row.key}
              className={`border-b border-grey-100 align-top ${row.highlight ? 'bg-safety-yellow/10' : ''}`}
            >
              {row.cells.map((cell, index) => (
                <td
                  key={index}
                  className={`px-3 py-1.5 ${index === 0 ? 'pl-5' : ''} ${typeof cell === 'string' ? 'font-mono break-all' : ''}`}
                >
                  {cell}
                </td>
              ))}
            </tr>
          ))}
          {rows.length === 0 && (
            <tr>
              <td colSpan={head.length} className="px-5 py-4 text-grey-600">
                {empty}
              </td>
            </tr>
          )}
        </tbody>
      </table>
    </div>
  )
}
