import { useEffect, useRef, useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import type { GameLoopHealth } from '@synapticon/motion-master-client'
import PageHeader from '../components/PageHeader'
import Callout from '../components/Callout'
import GameLoopExplainer from '../components/GameLoopExplainer'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline } from '../utils/styles'

// Poll cadence. The endpoint reports cumulative averages plus raw counters + a server timestamp, so
// we diff successive polls to derive the *instantaneous* rate the loop is achieving right now. One
// second is frequent enough to feel live without spamming the RT-adjacent HTTP thread.
const POLL_INTERVAL_MS = 1000

// A loop meeting its period sits within a hair of targetHz. We flag a sustained shortfall below this
// fraction as "not keeping up" — the coarse-timer degradation described in CLAUDE.md.
const KEEPING_UP_FRACTION = 0.99

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
      <p className="eyebrow text-grey-500 mb-2">{label}</p>
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

export default function GameLoopPage() {
  const { api } = useConnection()

  const query = useQuery({
    queryKey: ['gameLoop'],
    queryFn: () => api.getGameLoop(),
    refetchInterval: POLL_INTERVAL_MS,
  })

  const health = query.data?.data

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
      <div className="p-4 sm:p-8 space-y-6">
        <GameLoopExplainer />

        <div className="flex items-center justify-between gap-4">
          {health ? (
            keepingUp && !skipping ? (
              <p className="text-xs text-status-good">
                Loop is meeting its {formatHz(health.targetHz)} target.
              </p>
            ) : (
              <p className="text-xs text-status-bad font-medium">
                Loop is running below its {formatHz(health.targetHz)} target.
              </p>
            )
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
            {skipping && (
              <Callout variant="warning">
                The loop is skipping cycles
                {instant && instant.skipHz >= 1 ? ` (~${Math.round(instant.skipHz)}/s)` : ''} to stay
                on its grid — this machine cannot sustain the configured{' '}
                {health.periodUs} µs period. Each executed cycle stays phase-locked to a real grid
                point, so there is no drift or burst, but EtherCAT drives get a fresh frame in only
                some sync windows. Raise the configured period (e.g. to 2 ms) so the loop can meet
                its grid.
              </Callout>
            )}

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
                tone={health.skippedCycles > 0 ? 'bad' : 'good'}
                title="Cycles skipped to catch up after overruns/stalls since start"
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
          </>
        )}
      </div>
    </div>
  )
}
