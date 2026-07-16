import { useEffect, useRef, useState } from 'react'
import { useMutation, useQuery } from '@tanstack/react-query'
import type { GameLoopHealth } from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import Callout from '../components/Callout'
import GameLoopExplainer from '../components/GameLoopExplainer'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

const inputCls = 'border border-grey-300 px-2 py-1 text-xs w-28 font-mono bg-white'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

// Flatten the client's nested {error:{error}} / {status} shape into a single message.
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
      return `HTTP ${(err as { status: number }).status}`
    }
  }
  return 'Unknown error'
}

// Poll cadence. The endpoint reports cumulative averages plus raw counters + a server timestamp, so
// we diff successive polls to derive the *instantaneous* rate the loop is achieving right now. One
// second is frequent enough to feel live without spamming the RT-adjacent HTTP thread.
const POLL_INTERVAL_MS = 1000

// A loop meeting its period sits within a hair of targetHz. We flag a sustained shortfall below this
// fraction as "not keeping up" — the coarse-timer degradation described in CLAUDE.md.
const KEEPING_UP_FRACTION = 0.95

// Format a hertz figure with a sensible number of decimals for the magnitude.
function formatHz(hz: number): string {
  if (hz >= 100) return `${hz.toFixed(1)} Hz`
  if (hz >= 1) return `${hz.toFixed(2)} Hz`
  return `${hz.toFixed(3)} Hz`
}

// Format a nanosecond duration compactly: bare ns under a microsecond, then µs / ms.
function formatNs(ns: number): string {
  if (ns < 1_000) return `${ns} ns`
  if (ns < 1_000_000) return `${(ns / 1_000).toFixed(2)} µs`
  return `${(ns / 1_000_000).toFixed(2)} ms`
}

function Metric({
  label,
  value,
  sub,
  tone = 'normal',
  title,
}: {
  label: string
  value: React.ReactNode
  sub?: React.ReactNode
  tone?: 'normal' | 'good' | 'bad' | 'muted'
  title?: string
}) {
  const valueTone =
    tone === 'good'
      ? 'text-status-good'
      : tone === 'bad'
        ? 'text-status-bad'
        : tone === 'muted'
          ? 'text-grey-400'
          : 'text-grey-900'
  return (
    <div className="border border-grey-200 p-4" title={title}>
      <p className="eyebrow text-grey-500 mb-2 cursor-help">{label}</p>
      <p className={`font-display text-3xl font-light tabular-nums ${valueTone}`}>{value}</p>
      {sub && <p className="text-xs text-grey-600 mt-1">{sub}</p>}
    </div>
  )
}

function RtFlag({ ok, children }: { ok: boolean; children: React.ReactNode }) {
  return (
    <span className={`font-mono ${ok ? 'text-status-good' : 'text-grey-500'}`}>
      {ok ? 'yes' : 'no'} <span className="text-grey-500">{children}</span>
    </span>
  )
}

export default function ServerGameLoopPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['gameLoop'],
    queryFn: () => api.getGameLoop(),
    refetchInterval: POLL_INTERVAL_MS,
  })

  const health = query.data?.data

  // Cycle-period control. Prefill the input from the loop's current period once, then leave it under
  // the user's control (the 1 s poll must not clobber what they are typing).
  const [periodInput, setPeriodInput] = useState('')
  const [periodError, setPeriodError] = useState<string | null>(null)
  const prefilled = useRef(false)
  useEffect(() => {
    if (health && !prefilled.current) {
      setPeriodInput(String(health.periodUs))
      prefilled.current = true
    }
  }, [health])

  const setPeriodMutation = useMutation({
    mutationFn: (periodUs: number) => api.setGameLoopPeriod({ periodUs }),
  })

  function applyPeriod() {
    setPeriodError(null)
    const periodUs = Number(periodInput)
    if (!Number.isInteger(periodUs) || periodUs <= 0) {
      setPeriodError('Enter a whole number of microseconds greater than 0.')
      return
    }
    setPeriodMutation.mutate(periodUs, {
      onSuccess: () => {
        // The server reset its health counters for the new period, so drop our diff baseline —
        // otherwise the next poll diffs the fresh (smaller) counters against the pre-change sample
        // and renders a one-tick bogus rate. Same reset as the reconnect path below.
        prev.current = null
        setInstant(null)
        void query.refetch()
      },
      onError: (err) => setPeriodError(apiError(err)),
    })
  }

  // Derive the instantaneous rate by diffing this sample against the previous one: how many cycles
  // the loop actually ran, over the real wall-clock interval between the two server timestamps. This
  // catches a stall the cumulative achievedHz would smooth away over a long uptime.
  const prev = useRef<GameLoopHealth | null>(null)
  const [instant, setInstant] = useState<{ hz: number; skipHz: number } | null>(null)

  useEffect(() => {
    if (!health) return
    const last = prev.current
    if (last && health.timestampUs > last.timestampUs) {
      const dtSec = (health.timestampUs - last.timestampUs) / 1_000_000
      if (dtSec > 0) {
        const hz = (health.executedCycles - last.executedCycles) / dtSec
        const skipHz = (health.skippedCycles - last.skippedCycles) / dtSec
        setInstant({ hz: Math.max(0, hz), skipHz: Math.max(0, skipHz) })
      }
    }
    prev.current = health
  }, [health])

  // Reset the diff baseline whenever the connection drops so a stale sample can't produce a bogus
  // rate across a reconnect.
  useEffect(() => {
    if (query.isError) {
      prev.current = null
      setInstant(null)
    }
  }, [query.isError])

  const keepingUp = health ? health.achievedHz >= health.targetHz * KEEPING_UP_FRACTION : false
  const skipping = instant ? instant.skipHz >= 1 : health ? health.skippedCycles > 0 : false

  return (
    <div>
      <PageHeader
        eyebrow="Server"
        title="Game Loop"
        description={
          <>
            Real-time health of the cyclic loop that drives EtherCAT process-data exchange. The loop
            targets a fixed period; on a real-time host it holds it almost exactly, but on stock
            Windows / macOS the OS timer may be too coarse to sustain a 1 ms cycle — the loop then
            skips grid points to stay phase-locked rather than drift or burst. A steadily rising skip
            count means the configured period is too aggressive for this machine; raise it. Refreshes
            every second.
          </>
        }
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <GameLoopExplainer />

        {/* Always-present status line — the skip state changes its text/colour in this fixed slot
            rather than inserting a callout that shifts the tiles below. */}
        <div className="flex items-start justify-between gap-4">
          {health ? (
            <p
              className={`text-xs flex items-start gap-2 ${
                skipping || !keepingUp ? 'text-status-bad font-medium' : 'text-status-good'
              }`}
            >
              <span
                aria-hidden
                className={`mt-1 shrink-0 inline-block w-2 h-2 rounded-full ${
                  skipping || !keepingUp ? 'bg-status-bad' : 'bg-status-good'
                }`}
              />
              <span>
                {skipping
                  ? `Loop is skipping cycles${
                      instant && instant.skipHz >= 1 ? ` (~${Math.round(instant.skipHz)}/s)` : ''
                    } — this machine can’t sustain the ${health.periodUs} µs period; raise it.`
                  : keepingUp
                    ? `Loop is meeting its ${formatHz(health.targetHz)} target.`
                    : `Loop is running below its ${formatHz(health.targetHz)} target.`}
              </span>
            </p>
          ) : (
            <span />
          )}
          <button onClick={() => query.refetch()} disabled={query.isFetching} className={btnOutline}>
            {query.isFetching ? 'Loading…' : 'Refresh'}
          </button>
        </div>

        {query.isError && (
          <Callout variant="error">
            Failed to load game-loop health. Check that the server is reachable.
          </Callout>
        )}

        {query.isFetching && !health && <p className="text-xs text-grey-600">Loading…</p>}

        {health && (
          <>
            {!health.schedFifo && (
              <Callout variant="info">
                The loop is not running with real-time scheduling
                {health.memLocked ? '' : ' and its memory is not pinned'}. This is expected on
                Windows (which never attempts it) and on Linux / macOS without{' '}
                <code className="font-mono">CAP_SYS_NICE</code>
                {health.memLocked ? '' : ' / CAP_IPC_LOCK'}. Timing stays scheduler-bound and may
                jitter under load, but the loop still runs.
              </Callout>
            )}

            <div className="grid grid-cols-1 sm:grid-cols-2 lg:grid-cols-3 gap-4">
              <Metric
                label="Achieved rate"
                value={formatHz(health.achievedHz)}
                sub={`target ${formatHz(health.targetHz)} · ${health.periodUs} µs period`}
                tone={keepingUp ? 'good' : 'bad'}
                title="Cumulative average since the loop started (executedCycles ÷ uptime)"
              />
              <Metric
                label="Live rate"
                value={instant ? formatHz(instant.hz) : '—'}
                sub="this poll, diffed from the last"
                tone={instant ? (instant.hz >= health.targetHz * KEEPING_UP_FRACTION ? 'good' : 'bad') : 'muted'}
                title="Instantaneous rate: executed cycles between the last two polls ÷ elapsed server time"
              />
              <Metric
                label="Skipped cycles"
                value={health.skippedCycles.toLocaleString()}
                sub={
                  instant && instant.skipHz >= 1
                    ? `+${Math.round(instant.skipHz)}/s — not keeping up`
                    : 'grid points dropped after overruns'
                }
                tone={
                  instant && instant.skipHz >= 1 ? 'bad' : health.skippedCycles > 0 ? 'muted' : 'good'
                }
                title="Cumulative cycles skipped since start (the value); highlighted red only while the loop is currently skipping (≥1/s)"
              />
              <Metric
                label="Executed cycles"
                value={health.executedCycles.toLocaleString()}
                sub="loop iterations run since start"
                title="Loop iterations actually executed since start"
              />
              <Metric
                label="Task time"
                value={formatNs(health.lastExecNs)}
                sub={`max ${formatNs(health.maxExecNs)} · avg ${formatNs(health.avgExecNs)}`}
                tone="normal"
                title="Per-cycle task-execution time — the work done inside each cycle, excluding the wait"
              />
              <Metric
                label="RT scheduling"
                value={
                  <span className="text-lg space-y-1 flex flex-col">
                    <RtFlag ok={health.schedFifo}>SCHED_FIFO</RtFlag>
                    <RtFlag ok={health.memLocked}>mlockall</RtFlag>
                  </span>
                }
                title="Whether the loop acquired real-time priority and pinned its memory"
              />
            </div>

            <div className="border border-grey-200 p-4 space-y-3">
              <p className="eyebrow text-grey-500">Cycle period</p>
              <p className="text-xs text-grey-600">
                Retimes the running loop to a new period. Takes effect within one cycle. The change
                is transient — it is not saved to the config file, so a restart reverts to the
                configured value. If the skip count above climbs steadily, raise the period (e.g.
                1000 → 2000 µs) until the loop meets its grid. Applying a period resets the counters
                above so you can see straight away whether it helped.{' '}
                <span className="text-grey-700">
                  Only the master cadence changes: the recorder ring is not resized and drive
                  watchdogs are not touched, so raising the period toward a drive&rsquo;s PDO/SM
                  watchdog window can fault that drive — change it while drives are not enabled if
                  unsure.
                </span>
              </p>
              <div className="flex items-center gap-2">
                <input
                  className={inputCls}
                  value={periodInput}
                  inputMode="numeric"
                  placeholder={String(health.periodUs)}
                  onChange={(e) => {
                    setPeriodInput(e.target.value)
                    setPeriodMutation.reset()
                    setPeriodError(null)
                  }}
                />
                <span className="text-xs text-grey-500">µs</span>
                <button
                  className={btnCls}
                  onClick={applyPeriod}
                  disabled={setPeriodMutation.isPending}
                >
                  {setPeriodMutation.isPending ? 'Applying…' : 'Apply period'}
                </button>
              </div>
              {periodError && <p className="text-xs text-status-bad">{periodError}</p>}
              {setPeriodMutation.isSuccess && !periodError && (
                <p className="text-xs text-status-good">
                  Period applied — loop now targeting {formatHz(health.targetHz)}.
                </p>
              )}
            </div>
          </>
        )}
      </div>
    </div>
  )
}
