import { useEffect, useState } from 'react'
import { useMutation, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage, formatHex, type Cia402Status } from '@synapticon/motion-master-client'
import type { DeviceBarContext } from './DeviceBar'
import { barBtnOutline, barBtnPrimary, barSelect, labelCls } from './deviceBarStyles'
import SlavePositionBadge from './SlavePositionBadge'
import StoreParametersButton from './StoreParametersButton'
import { useConnection } from '../contexts/ConnectionContext'
import { cia402StatusKey } from '../hooks/useCia402Status'
import { findMode, useOperationModes } from '../hooks/useOperationModes'

// Per-state badge colour: green when fully enabled, amber for the transient quick-stop and
// fault-reaction states, red for a latched fault, neutral otherwise. Matches the AL-state badges in
// the sidebar in spirit — a state reads the same colour wherever it is shown.
const STATE_BADGE: Record<Cia402Status['state'], string> = {
  NotReadyToSwitchOn: 'bg-grey-200 text-grey-700',
  SwitchOnDisabled: 'bg-grey-200 text-grey-700',
  ReadyToSwitchOn: 'bg-ocean text-white',
  SwitchedOn: 'bg-status-info text-white',
  OperationEnabled: 'bg-green-600 text-white',
  QuickStopActive: 'bg-status-warn text-grey-900',
  FaultReactionActive: 'bg-status-warn text-grey-900',
  Fault: 'bg-syn-red text-white',
}

// The CiA402 states a master may ask a drive to reach, ordered as the state machine climbs. The
// other three — Not Ready To Switch On, Fault Reaction Active and Fault — are entered by the drive
// itself and cannot be requested, so they are not offered.
const CIA402_TARGETS = [
  { value: 'SwitchOnDisabled', label: 'Switch On Disabled' },
  { value: 'ReadyToSwitchOn', label: 'Ready To Switch On' },
  { value: 'SwitchedOn', label: 'Switched On' },
  { value: 'OperationEnabled', label: 'Operation Enabled' },
  { value: 'QuickStopActive', label: 'Quick Stop Active' },
] as const
type Cia402TargetState = (typeof CIA402_TARGETS)[number]['value']

interface DeviceStatusRowProps {
  context: DeviceBarContext
}

/**
 * The first row of DeviceBar: which device this is and what its drive is doing right now — the
 * CiA402 state, the mode, the statusword and the controlword — with the controls worth having
 * within reach on every device page: the mode and state selects, a quick stop, and the fault reset
 * that follows one.
 *
 * Everything here is read-only chrome apart from those controls, so it is safe on every device
 * route including the ones that have nothing to do with motion.
 */
export default function DeviceStatusRow({ context }: DeviceStatusRowProps) {
  const {
    slavePosition,
    productName,
    isCia402,
    mailboxActive,
    status,
    statusError,
    canCommand,
    whyDisabled,
  } = context
  const { api } = useConnection()
  const queryClient = useQueryClient()

  // The drive's own mode table, so a manufacturer mode reads as itself. The status response names
  // only the standard modes — it is CiA402's view — so 0x6061 = -2 arrives as "Unknown" and is
  // resolved here to "Diagnostics".
  const modesQuery = useOperationModes(slavePosition, isCia402 && mailboxActive)
  const activeMode = status ? findMode(modesQuery.data?.modes, status.modeOfOperation) : undefined
  const modeName = activeMode?.name ?? status?.modeName

  const commandMutation = useMutation({
    mutationFn: (command: 'quickStop' | 'faultReset') =>
      api.runCia402Command(slavePosition, { command }),
    onSuccess: (r) => queryClient.setQueryData(cia402StatusKey(slavePosition), r.data),
  })

  // Both selects seed from what the drive currently reports and then belong to the user until they
  // press the button beside them — the seed-once rule the Motion page's inputs follow. A select
  // that rewrote itself under the cursor as the drive changed would be unusable.
  const [selectedMode, setSelectedMode] = useState<number | null>(null)
  const [selectedState, setSelectedState] = useState<Cia402TargetState | null>(null)
  useEffect(() => {
    if (selectedMode === null && status) setSelectedMode(status.modeOfOperation)
  }, [status, selectedMode])
  useEffect(() => {
    if (selectedState !== null || !status) return
    // Seed with where the drive is, when that is somewhere it could also be sent. From a state it
    // cannot be sent to — Fault, or still starting up — seed with the one that gets it out.
    setSelectedState(
      CIA402_TARGETS.some((t) => t.value === status.state)
        ? (status.state as Cia402TargetState)
        : 'SwitchOnDisabled',
    )
  }, [status, selectedState])

  const setModeMutation = useMutation({
    mutationFn: (mode: number) => api.setCia402OperationMode(slavePosition, { mode }),
    onSuccess: (r) => queryClient.setQueryData(cia402StatusKey(slavePosition), r.data),
  })

  const transitionMutation = useMutation({
    // The server walks whatever transitions it takes and answers with where the drive landed, so
    // there is nothing to sequence here.
    mutationFn: (state: Cia402TargetState) =>
      api.transitionToCia402State(slavePosition, { state }),
    onSuccess: (r) => queryClient.setQueryData(cia402StatusKey(slavePosition), r.data),
  })

  // Writing 0x6060 is an SDO, so it needs a mailbox — PRE-OP or above. It does not need OP, unlike
  // anything that moves the state machine: the drive adopts a requested mode without advancing.
  const canSetMode = isCia402 && mailboxActive
  const modes = modesQuery.data?.modes ?? []
  const standardModes = modes.filter((m) => m.kind === 'standard')
  const manufacturerModes = modes.filter((m) => m.kind === 'manufacturer')

  return (
    <div className="flex flex-wrap items-center justify-between gap-x-6 gap-y-2 px-4 py-2 sm:px-8">
      <div className="flex flex-wrap items-center gap-x-4 gap-y-2">
        <div className="flex items-center gap-2">
          <SlavePositionBadge position={slavePosition} />
          <span className="font-display text-sm tracking-wide text-grey-900">{productName}</span>
          <StoreParametersButton slavePosition={slavePosition} mailboxActive={mailboxActive} />
        </div>

        {isCia402 && (
          <>
            {statusError ? (
              <span className="truncate text-xs text-syn-red" title={apiErrorMessage(statusError)}>
                {apiErrorMessage(statusError)}
              </span>
            ) : !status ? (
              <span className="text-xs text-grey-500">
                {mailboxActive ? 'Reading…' : 'No mailbox'}
              </span>
            ) : (
              <div className="flex flex-wrap items-center gap-x-4 gap-y-1 border-l border-grey-200 pl-4">
                <span
                  className={`inline-block rounded-sm px-2.5 py-0.5 text-xs ${STATE_BADGE[status.state]}`}
                  title="CiA402 state — the device-control state machine's state, decoded from the statusword (0x6041)"
                >
                  {status.state}
                </span>
                {status.errorDescription && (
                  <span
                    className="font-mono text-xs text-syn-red"
                    title="Error description — the drive's own short description of its last fault (0x203F:01)."
                  >
                    {status.errorDescription}
                  </span>
                )}
                <div className="flex items-baseline gap-1.5">
                  <span className={labelCls}>Mode</span>
                  <span className="text-xs" title={activeMode?.label}>
                    {modeName} ({status.modeOfOperation})
                  </span>
                </div>
                <div className="flex items-baseline gap-1.5">
                  <span className={labelCls}>Statusword</span>
                  <span className="font-mono text-xs">{formatHex(status.statusword, 4)}</span>
                </div>
                <div className="flex items-baseline gap-1.5">
                  <span className={labelCls}>Controlword</span>
                  <span className="font-mono text-xs">{formatHex(status.controlword, 4)}</span>
                </div>
              </div>
            )}
          </>
        )}
      </div>

      {isCia402 && (
        <div className="flex items-center gap-2">
          {/* Mode, then state, then the two standing commands — separated by a divider because the
              first two ask a question and wait, while the last two act at once. */}
          <div className="flex items-center gap-1.5">
            <select
              aria-label="Operation mode"
              className={barSelect}
              disabled={!canSetMode || setModeMutation.isPending}
              value={selectedMode ?? status?.modeOfOperation ?? 0}
              onChange={(e) => setSelectedMode(Number(e.target.value))}
              title={
                canSetMode
                  ? 'Operation mode (0x6060) to request. Unsupported standard modes are listed but cannot be chosen; the manufacturer modes are the drive vendor’s own.'
                  : `Operation mode — unavailable: ${whyDisabled}.`
              }
            >
              {/* Name and value only: a native select is as wide as its longest option, and this
                  one sits in a status bar next to five other controls. A mode the drive does not
                  advertise is greyed out, which is signal enough here; the vendor's deprecated
                  flag is spelled out on the Motion page's Mode card, where there is room. */}
              <optgroup label="Standard">
                {standardModes.map((m) => (
                  <option key={m.value} value={m.value} disabled={m.supported === false}>
                    {m.name} ({m.value})
                  </option>
                ))}
              </optgroup>
              {manufacturerModes.length > 0 && (
                <optgroup label="Manufacturer-specific">
                  {manufacturerModes.map((m) => (
                    <option key={m.value} value={m.value}>
                      {m.name} ({m.value})
                    </option>
                  ))}
                </optgroup>
              )}
            </select>
            <button
              type="button"
              className={barBtnOutline}
              disabled={!canSetMode || selectedMode === null || setModeMutation.isPending}
              onClick={() => selectedMode !== null && setModeMutation.mutate(selectedMode)}
              title={
                canSetMode
                  ? 'Set mode — request this operation mode (0x6060). The drive adopts it, reflected as the active mode (0x6061), once accepted. Change it with the drive disabled.'
                  : `Set mode — unavailable: ${whyDisabled}.`
              }
            >
              Set mode
            </button>
          </div>

          <div className="flex items-center gap-1.5">
            <select
              aria-label="CiA402 state"
              className={barSelect}
              disabled={!canCommand || transitionMutation.isPending}
              value={selectedState ?? 'SwitchOnDisabled'}
              onChange={(e) => setSelectedState(e.target.value as Cia402TargetState)}
              title={
                canCommand
                  ? 'CiA402 state to bring the drive to. The server walks whatever transitions that takes.'
                  : `CiA402 state — unavailable: ${whyDisabled}.`
              }
            >
              {CIA402_TARGETS.map((t) => (
                <option key={t.value} value={t.value}>
                  {t.label}
                </option>
              ))}
            </select>
            <button
              type="button"
              className={barBtnOutline}
              disabled={!canCommand || selectedState === null || transitionMutation.isPending}
              onClick={() => selectedState !== null && transitionMutation.mutate(selectedState)}
              title={
                canCommand
                  ? 'Transition — bring the drive to the chosen state, issuing every transition it takes and clearing a fault first if there is one. Asking for Operation Enabled from Quick Stop Active leaves that quick stop.'
                  : `Transition — unavailable: ${whyDisabled}.`
              }
            >
              Transition
            </button>
          </div>

          <span className="mx-1 h-[22px] w-px shrink-0 bg-grey-200" aria-hidden="true" />

          {(setModeMutation.isError || transitionMutation.isError) && (
            <span
              className="max-w-[28ch] truncate text-xs text-status-bad"
              title={apiErrorMessage(setModeMutation.error ?? transitionMutation.error)}
            >
              {apiErrorMessage(setModeMutation.error ?? transitionMutation.error)}
            </span>
          )}
          {commandMutation.isError && (
            <span
              className="max-w-[28ch] truncate text-xs text-status-bad"
              title={apiErrorMessage(commandMutation.error)}
            >
              {apiErrorMessage(commandMutation.error)}
            </span>
          )}
          <button
            type="button"
            className={barBtnPrimary}
            disabled={!canCommand || commandMutation.isPending}
            onClick={() => commandMutation.mutate('quickStop')}
            title={
              canCommand
                ? 'Quick stop — command a controlled stop (controlword bit 2 cleared). The drive decelerates on its quick-stop ramp and lands in Quick Stop Active; recover with Disable then Enable on the Motion page.'
                : `Quick stop — unavailable: ${whyDisabled}.`
            }
          >
            Quick stop
          </button>
          <button
            type="button"
            className={barBtnOutline}
            disabled={!canCommand || commandMutation.isPending}
            onClick={() => commandMutation.mutate('faultReset')}
            title={
              canCommand
                ? 'Reset fault — clear a latched fault (controlword bit 7), returning the drive to Switch On Disabled. It clears the latch, not the cause: a fault whose cause is still present latches again immediately.'
                : `Reset fault — unavailable: ${whyDisabled}.`
            }
          >
            Reset fault
          </button>
        </div>
      )}
    </div>
  )
}
