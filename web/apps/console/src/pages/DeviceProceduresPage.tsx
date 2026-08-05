import { useEffect, useMemo, useState } from 'react'
import { Navigate, NavLink, useParams } from 'react-router'
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import type {
  ProcedureListing,
  ProcedureSnapshot,
  ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import DevicePageHeader from '../components/DevicePageHeader'
import ProceduresExplainer from '../components/ProceduresExplainer'
import ProcedureParameters, {
  initialParameterValues,
  parseParameterValues,
  type ParameterValues,
} from '../components/ProcedureParameters'
import { useConnection } from '../contexts/ConnectionContext'

// One shared control height for every interactive control, matching the other device pages: this
// theme's spacing scale is geometric (h-9 = 6rem = 96px!), so the height has to be explicit px.
const btnCls =
  'inline-flex items-center justify-center bg-syn-red text-white px-4 h-[38px] text-xs ' +
  'hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'
const btnGhostCls =
  'inline-flex items-center justify-center border border-grey-300 text-grey-700 px-4 h-[38px] ' +
  'text-xs hover:bg-grey-50 disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer ' +
  'transition-colors'
const statLabelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display'

// Overall run status → badge colour. Mirrors the AL-state and CiA402 badges elsewhere: green for a
// clean finish, red for a failure, amber for a user cancel (not an error, but not success either),
// ocean while working, neutral when nothing has run.
const STATUS_BADGE: Record<ProcedureSnapshot['status'], string> = {
  idle: 'bg-grey-200 text-grey-700',
  running: 'bg-ocean text-white',
  succeeded: 'bg-green-600 text-white',
  failed: 'bg-syn-red text-white',
  cancelled: 'bg-status-warn text-grey-900',
}

// Per-step status → dot colour, for the compact step list.
const STEP_DOT: Record<ProgressStep['status'], string> = {
  idle: 'bg-grey-300',
  running: 'bg-ocean',
  succeeded: 'bg-green-600',
  failed: 'bg-syn-red',
}

// Surfaces the node layer's error string from a failed request (matching the other device pages).
function apiError(err: unknown): string {
  if (err && typeof err === 'object') {
    if ('error' in err) {
      const inner = (err as { error: unknown }).error
      if (inner && typeof inner === 'object' && 'error' in inner) {
        return String((inner as { error: unknown }).error)
      }
      if (typeof inner === 'string') return inner
    }
    if ('status' in err && typeof (err as { status: unknown }).status === 'number') {
      const { status } = err as { status: number }
      return `HTTP ${status}`
    }
  }
  if (err instanceof Error) return err.message
  return 'Unknown error'
}

// Wall-clock time of a run boundary, to the millisecond — 11:01:28.762. The precision is the point:
// plenty of procedures finish inside a second, so second-resolution timestamps would render a start
// and its finish as the same instant. Every component is named explicitly because
// fractionalSecondDigits counts as a component option, and passing it alone suppresses the default
// hour/minute/second and formats the fraction on its own.
function formatTime(epochMs?: number): string {
  if (epochMs === undefined) return '—'
  return new Date(epochMs).toLocaleTimeString([], {
    hour12: false,
    hour: '2-digit',
    minute: '2-digit',
    second: '2-digit',
    fractionalSecondDigits: 3,
  })
}

// How long a finished run took, or how long the current one has been going.
function formatDuration(ms: number): string {
  if (ms < 1000) return `${ms} ms`
  return `${(ms / 1000).toFixed(ms < 10_000 ? 2 : 1)} s`
}

export default function DeviceProceduresPage() {
  const { deviceId, procedureName } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const listKey = ['procedures', slavePosition]
  const listQuery = useQuery({
    queryKey: listKey,
    queryFn: () => api.listProcedures(slavePosition).then(r => r.data),
    // One request carries every procedure's descriptor AND its snapshot, so the whole page — list,
    // detail and live progress — is driven by this single query. Poll only while something is
    // actually running; a page of finished results needs no traffic at all.
    refetchInterval: query =>
      (query.state.data ?? []).some(l => l.snapshot.status === 'running') ? 300 : false,
    retry: false,
  })

  const listings = listQuery.data ?? []
  const selected = listings.find(l => l.descriptor.name === procedureName)

  const startMutation = useMutation({
    mutationFn: ({ name, body }: { name: string; body?: Record<string, unknown> }) =>
      api.startProcedure(slavePosition, name, body ?? {}),
    // The 202 body is the initial snapshot, but the list is what this page renders — refetch so the
    // run appears with its bumped runCount, and the poll above takes over from there.
    onSettled: () => queryClient.invalidateQueries({ queryKey: listKey }),
  })

  const cancelMutation = useMutation({
    mutationFn: (name: string) => api.cancelProcedure(slavePosition, name),
    onSettled: () => queryClient.invalidateQueries({ queryKey: listKey }),
  })

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Procedures"
        description="Operations you can run on this device, each handled as a background job: start it, watch its steps, cancel it if you need to. The result stays until the next run."
      />

      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <ProceduresExplainer />

        {listQuery.isError ? (
          <Callout variant="error">{apiError(listQuery.error)}</Callout>
        ) : listQuery.isPending ? (
          <p className="text-sm text-grey-500">Reading procedures…</p>
        ) : listings.length === 0 ? (
          <Callout variant="info">
            This device has no procedures. The list is served per device — a procedure is offered only
            where it applies, so a slave from another vendor reports none.
          </Callout>
        ) : (
          // Master-detail: pick a procedure on the left, work it on the right. The selection lives in
          // the URL (/devices/:id/procedures/:name) rather than in component state, so a specific
          // procedure can be linked to, survives a reload, and the browser's back button works.
          <div className="grid grid-cols-1 lg:grid-cols-[15rem_1fr] gap-6 items-start">
            <nav className="border border-grey-200">
              <p className="px-4 py-3 border-b border-grey-200 eyebrow">Procedure</p>
              <ul>
                {listings.map(listing => (
                  <li key={listing.descriptor.name}>
                    <ProcedureNavItem
                      slavePosition={slavePosition}
                      listing={listing}
                      selected={listing.descriptor.name === procedureName}
                    />
                  </li>
                ))}
              </ul>
            </nav>

            {procedureName === undefined ? (
              // Nothing selected: open the first one rather than showing a prompt that costs a click
              // for no information. `replace` keeps it out of the history, so Back leaves the page.
              <Navigate
                to={`/devices/${slavePosition}/procedures/${listings[0].descriptor.name}`}
                replace
              />
            ) : selected === undefined ? (
              <Callout variant="error">
                This device has no procedure named <code>{procedureName}</code>. Pick one from the
                list.
              </Callout>
            ) : (
              <ProcedureDetail
                key={selected.descriptor.name}
                slavePosition={slavePosition}
                listing={selected}
                // The busy claim is per device, not per procedure, so a run on any of them refuses a
                // start here (HTTP 409). Say so on the button rather than letting the user find out.
                busyWith={listings.find(
                  l => l.snapshot.status === 'running' && l.descriptor.name !== selected.descriptor.name,
                )?.descriptor.title}
                onStart={body => startMutation.mutate({ name: selected.descriptor.name, body })}
                onCancel={() => cancelMutation.mutate(selected.descriptor.name)}
                starting={startMutation.isPending}
                cancelling={cancelMutation.isPending}
                requestError={
                  startMutation.isError
                    ? apiError(startMutation.error)
                    : cancelMutation.isError
                      ? apiError(cancelMutation.error)
                      : null
                }
              />
            )}
          </div>
        )}
      </div>
    </div>
  )
}

// One entry in the procedure list: its title, and a dot carrying the status of its last run so the
// list doubles as an at-a-glance summary of the device.
function ProcedureNavItem({
  slavePosition,
  listing,
  selected,
}: {
  slavePosition: number
  listing: ProcedureListing
  selected: boolean
}) {
  const { status } = listing.snapshot
  return (
    <NavLink
      to={`/devices/${slavePosition}/procedures/${listing.descriptor.name}`}
      className={`flex items-center justify-between gap-2 px-4 py-2.5 text-xs border-l-2 transition-colors ${
        selected
          ? 'border-syn-red bg-grey-50 text-grey-900'
          : 'border-transparent text-grey-600 hover:bg-grey-50 hover:text-grey-900'
      }`}
    >
      <span className="font-display uppercase tracking-wide">{listing.descriptor.title}</span>
      <span
        title={status}
        aria-label={status}
        className={`h-2 w-2 shrink-0 rounded-full ${STEP_DOT[statusDot(status)]} ${
          status === 'running' ? 'animate-pulse' : ''
        }`}
      />
    </NavLink>
  )
}

// The list dot reuses the per-step palette, so the five overall statuses map onto its four colours:
// a cancelled run reads as a failure at a glance (it did not produce a result), with the exact status
// in the tooltip and the detail panel.
function statusDot(status: ProcedureSnapshot['status']): ProgressStep['status'] {
  switch (status) {
    case 'running':
      return 'running'
    case 'succeeded':
      return 'succeeded'
    case 'failed':
    case 'cancelled':
      return 'failed'
    default:
      return 'idle'
  }
}

function ProcedureDetail({
  slavePosition,
  listing,
  busyWith,
  onStart,
  onCancel,
  starting,
  cancelling,
  requestError,
}: {
  slavePosition: number
  listing: ProcedureListing
  /** Title of another procedure running on this device, which blocks a start here. */
  busyWith?: string
  onStart: (body?: Record<string, unknown>) => void
  onCancel: () => void
  starting: boolean
  cancelling: boolean
  requestError: string | null
}) {
  const { descriptor, snapshot } = listing
  const running = snapshot.status === 'running'

  // Elapsed time for a run in flight, ticking beside the button that started it. A finished run's
  // duration comes from its own timestamps and needs no timer.
  const [now, setNow] = useState(() => Date.now())
  useEffect(() => {
    if (!running) return
    const timer = setInterval(() => setNow(Date.now()), 100)
    return () => clearInterval(timer)
  }, [running])

  const duration = useMemo(() => {
    if (snapshot.startedAt === undefined) return null
    const end = snapshot.finishedAt ?? (running ? now : undefined)
    if (end === undefined) return null
    return formatDuration(Math.max(0, end - snapshot.startedAt))
  }, [snapshot.startedAt, snapshot.finishedAt, running, now])

  // The procedure's parameters, seeded from the defaults its descriptor declares. Held here (keyed
  // on the procedure by the parent's `key`) so switching procedures does not carry one procedure's
  // input into another.
  const [values, setValues] = useState<ParameterValues>(() =>
    initialParameterValues(descriptor.parameters),
  )
  const { body, errors } = parseParameterValues(descriptor.parameters, values)

  const canRun = running || starting || busyWith !== undefined ? false : body !== null

  function start() {
    if (body !== null) {
      onStart(body)
    }
  }

  return (
    <div className="space-y-6">
      {/* What it is — title, flags, description, and the caveats that apply before running it. */}
      <section className="border border-grey-200 p-5 space-y-4">
        <div className="flex flex-wrap items-center gap-3">
          <h3 className="text-sm font-display uppercase">{descriptor.title}</h3>
          <code className="text-[10px] text-grey-500">{descriptor.name}</code>
          {descriptor.movesMotor && (
            <span className="rounded-sm bg-status-warn/15 text-grey-800 px-2 py-0.5 text-[10px] uppercase tracking-wide font-display">
              Can move the motor
            </span>
          )}
          {descriptor.requiresEnabled && (
            <span className="rounded-sm bg-status-info/15 text-grey-800 px-2 py-0.5 text-[10px] uppercase tracking-wide font-display">
              Requires the drive enabled
            </span>
          )}
        </div>

        <p className="text-xs text-grey-600 max-w-3xl">{descriptor.description}</p>

        {/* Caveats are always a warning, never danger: they are what to know before running, not a
            destructive action being confirmed. Red would also make every motor-moving procedure
            shout, which spends the strongest emphasis the design has on the ordinary case. */}
        {descriptor.caveats.length > 0 && (
          <Callout variant="warning">
            <ul className="list-disc pl-4">
              {descriptor.caveats.map(caveat => (
                <li key={caveat}>{caveat}</li>
              ))}
            </ul>
          </Callout>
        )}

        {/* Parameters, rendered from what the descriptor declares rather than from a form written
            per procedure — so a new parameterized procedure is a row in the server's catalogue and
            nothing here. */}
        <ProcedureParameters
          parameters={descriptor.parameters}
          values={values}
          errors={errors}
          disabled={running}
          onChange={(name, value) => setValues(previous => ({ ...previous, [name]: value }))}
        />

        {/* Run / Cancel, with the elapsed or last-run duration beside the button that produced it. */}
        <div className="flex items-center justify-between gap-4 pt-1">
          <div className="flex flex-wrap gap-2">
            <button className={btnCls} disabled={!canRun} onClick={start}>
              {running ? 'Running…' : 'Run'}
            </button>
            <button className={btnGhostCls} disabled={!running || cancelling} onClick={onCancel}>
              Cancel
            </button>
          </div>
          {duration && (
            <div className="flex items-baseline gap-1.5">
              <span className={statLabelCls}>{running ? 'Elapsed' : 'Took'}</span>
              <span className="text-sm font-mono">{duration}</span>
            </div>
          )}
        </div>

        {busyWith && (
          <p className="text-xs text-grey-500">
            This device is busy running <strong>{busyWith}</strong>. One procedure runs per device at
            a time.
          </p>
        )}

        {requestError && <p className="text-status-bad text-xs">{requestError}</p>}
      </section>

      {/* How the run went — the decoded view of the snapshot. */}
      <section className="border border-grey-200 p-5 space-y-4">
        <h3 className="text-sm font-display uppercase">Last run</h3>

        <div className="flex flex-wrap items-center gap-x-6 gap-y-2">
          <span
            className={`inline-block rounded-sm px-3 py-1 text-sm ${STATUS_BADGE[snapshot.status]}`}
          >
            {snapshot.status}
          </span>
          <div className="flex items-baseline gap-1.5">
            <span className={statLabelCls}>Runs</span>
            <span className="text-sm font-mono">{snapshot.runCount}</span>
          </div>
          <div className="flex items-baseline gap-1.5">
            <span className={statLabelCls}>Started</span>
            <span className="text-sm font-mono">{formatTime(snapshot.startedAt)}</span>
          </div>
          <div className="flex items-baseline gap-1.5">
            <span className={statLabelCls}>Finished</span>
            <span className="text-sm font-mono">{formatTime(snapshot.finishedAt)}</span>
          </div>
        </div>

        {snapshot.status === 'idle' && (
          <p className="text-xs text-grey-500">
            This procedure has not run on this device since the last scan.
          </p>
        )}

        {/* A failure that belongs to no step — the device turning out not to be the kind the
            procedure needs, say — which would otherwise leave a failed run with nothing saying why. */}
        {snapshot.error && <Callout variant="error">{snapshot.error}</Callout>}

        <div className="border border-grey-200">
          <table className="w-full text-xs border-collapse">
            <thead>
              <tr className="border-b border-grey-200 bg-grey-50">
                <th className="w-px" />
                <th className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">
                  Step
                </th>
                <th className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">
                  Status
                </th>
                <th className="text-left px-3 py-2 font-display uppercase tracking-wide text-grey-600 font-medium">
                  Value
                </th>
              </tr>
            </thead>
            <tbody>
              {snapshot.steps.map(step => (
                <tr key={step.id} className="border-b border-grey-100 last:border-0 align-top">
                  <td className="pl-3 py-2">
                    <span
                      className={`inline-block h-2 w-2 rounded-full ${STEP_DOT[step.status]} ${
                        step.status === 'running' ? 'animate-pulse' : ''
                      }`}
                    />
                  </td>
                  <td className="px-3 py-2 font-mono">{step.id}</td>
                  <td className="px-3 py-2">{step.status}</td>
                  <td className="px-3 py-2">
                    {/* The shape of a step's value is per procedure by design, so it is shown as it
                        arrives rather than formatted per procedure. A failed step's `error` carries
                        the decoded reason — for an OS command, the named error code. */}
                    {step.value !== undefined && step.value !== null ? (
                      <code className="text-grey-800">{JSON.stringify(step.value)}</code>
                    ) : (
                      <span className="text-grey-400">—</span>
                    )}
                    {step.error && <p className="text-status-bad mt-1">{step.error}</p>}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      </section>

      {/* The wire view. This console is also the reference for anyone building a purpose-built UI on
          these endpoints, so the exact requests and the unformatted response are on the page — what
          you would poll, and precisely what you would get back. */}
      <section className="border border-grey-200 p-5 space-y-3">
        <h3 className="text-sm font-display uppercase">Wire</h3>
        <p className="text-xs text-grey-600 max-w-3xl">
          The three requests behind this panel, and the response body exactly as the snapshot above
          was decoded from. The whole client contract: <code>POST</code> to start, then read{' '}
          <code>GET</code> repeatedly for as long as it answers <code>running</code>, and stop once it
          does not. There is nothing to poll before a run or after one — this page follows the same
          rule, reading a few times a second while a run is in flight and not at all otherwise.
        </p>
        <dl className="grid grid-cols-[auto_1fr] gap-x-4 gap-y-1 text-xs font-mono text-grey-700">
          <dt className="text-grey-500">POST</dt>
          <dd>/api/devices/{slavePosition}/procedures/{descriptor.name}</dd>
          <dt className="text-grey-500">GET</dt>
          <dd>/api/devices/{slavePosition}/procedures/{descriptor.name}</dd>
          <dt className="text-grey-500">DELETE</dt>
          <dd>/api/devices/{slavePosition}/procedures/{descriptor.name}</dd>
        </dl>
        <pre className="border border-grey-200 bg-grey-50 p-3 text-xs font-mono overflow-x-auto">
          {JSON.stringify(snapshot, null, 2)}
        </pre>
      </section>
    </div>
  )
}
