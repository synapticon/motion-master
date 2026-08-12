import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage, formatHex, type Cia402Status } from '@synapticon/motion-master-client'
import SlavePositionBadge from './SlavePositionBadge'
import { useConnection } from '../contexts/ConnectionContext'
import { cia402StatusKey, useCia402Status } from '../hooks/useCia402Status'
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

const labelCls = 'text-[10px] uppercase tracking-wide text-grey-500 font-display'

// The two buttons share one **explicit** height and are inline-flex centred, the same rule every
// control pair in this app follows. Left to derive their height from padding they do not agree:
// only the outline variant carries a border, so the pair rendered a few pixels apart — the shared
// btnPrimary/btnOutline are safe side by side only where nothing else varies.
//
// 30px, not the 38px used for form controls: this is always-on chrome above every device page, so
// its height is permanently spent, and a button here sits beside a badge rather than an input.
const barBtn =
  'inline-flex items-center justify-center h-[30px] px-4 text-xs cursor-pointer ' +
  'transition-colors disabled:opacity-50 disabled:cursor-not-allowed'
const barBtnPrimary = `${barBtn} bg-syn-red text-white hover:bg-ocean`
const barBtnOutline = `${barBtn} border border-syn-red text-syn-red hover:bg-syn-red hover:text-white`

// The AL state a CiA402 command needs. The drive's state machine only advances while its statusword
// and controlword are exchanging as process data, which happens in OP and nowhere else — an SDO
// write to the controlword in PRE-OP lands in the object and is never acted on.
const OP_STATE = 8

// Mailbox-carrying AL states (PRE-OP, SAFE-OP, OP). Reading the status needs one; INIT has no
// mailbox and BOOT's is FoE-only. Mirrors the sidebar's own check.
const MAILBOX_ACTIVE_STATES = new Set([2, 4, 8])

interface DeviceStatusBarProps {
  slavePosition: number
}

/**
 * The bar that stays pinned to the top of every device page.
 *
 * It answers two questions that a long page otherwise loses: **which device am I working on** — the
 * sidebar scrolls away, and with a dozen devices its highlighted entry may be nowhere near the
 * viewport — and **what is this drive doing right now**. For a CiA402 drive it also carries the two
 * commands worth having within reach at all times rather than only on the Motion page: a quick stop,
 * and the fault reset that follows one.
 *
 * Everything here is read-only chrome apart from those two buttons, so it is safe on every device
 * route including the ones that have nothing to do with motion.
 */
export default function DeviceStatusBar({ slavePosition }: DeviceStatusBarProps) {
  const { api } = useConnection()
  const queryClient = useQueryClient()

  // Both queries are already owned elsewhere — the device list by the page header, the AL states by
  // the sidebar's 3-second poll — so these are extra observers on a warm cache rather than new
  // traffic.
  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    staleTime: Infinity,
  })
  const statesQuery = useQuery({
    queryKey: ['deviceStates'],
    queryFn: () => api.getDeviceStates(),
  })

  const device = devicesQuery.data?.data.find((d) => d.slavePosition === slavePosition)
  const alState = statesQuery.data?.data.find((s) => s.slavePosition === slavePosition)?.alState
  const isCia402 = device?.isCia402 ?? false
  const mailboxActive = alState !== undefined && MAILBOX_ACTIVE_STATES.has(alState)

  const statusQuery = useCia402Status(slavePosition, isCia402 && mailboxActive)
  const status = statusQuery.data

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

  // Commanding needs OP, for the reason on OP_STATE. Disabled rather than hidden: a button that
  // vanishes leaves you wondering where it went, where a disabled one with a reason tells you what
  // to fix.
  const canCommand = isCia402 && alState === OP_STATE
  const whyDisabled = !isCia402
    ? 'this device is not a CiA402 drive'
    : alState === undefined
      ? 'the device’s EtherCAT state is not known yet'
      : 'the device is not in the OP EtherCAT state, so its CiA402 state machine is not advancing'

  return (
    <div className="sticky top-0 z-20 flex flex-wrap items-center justify-between gap-x-6 gap-y-2 border-b border-grey-200 bg-white px-4 py-2 sm:px-8">
      <div className="flex flex-wrap items-center gap-x-4 gap-y-2">
        <div className="flex items-center gap-2">
          <SlavePositionBadge position={slavePosition} />
          <span className="font-display text-sm tracking-wide text-grey-900">
            {device?.productName ?? device?.name ?? `Device ${slavePosition}`}
          </span>
        </div>

        {isCia402 && (
          <>
            {statusQuery.isError ? (
              <span
                className="truncate text-xs text-syn-red"
                title={apiErrorMessage(statusQuery.error)}
              >
                {apiErrorMessage(statusQuery.error)}
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
