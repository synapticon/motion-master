import { useQuery } from '@tanstack/react-query'
import type { Cia402Status } from '@synapticon/motion-master-client'
import DeviceStatusRow from './DeviceStatusRow'
import { useConnection } from '../contexts/ConnectionContext'
import { useCia402Status } from '../hooks/useCia402Status'

// The AL state a CiA402 command needs. The drive's state machine only advances while its statusword
// and controlword are exchanging as process data, which happens in OP and nowhere else — an SDO
// write to the controlword in PRE-OP lands in the object and is never acted on.
export const OP_STATE = 8

// Mailbox-carrying AL states (PRE-OP, SAFE-OP, OP). Reading the status needs one; INIT has no
// mailbox and BOOT's is FoE-only. Mirrors the sidebar's own check.
const MAILBOX_ACTIVE_STATES = new Set([2, 4, 8])

/**
 * What every row of the bar reads: the device, its AL state, the CiA402 status poll, and whether
 * the drive can be commanded right now. Computed once in DeviceBar and handed down, so the rows
 * agree on one answer and the status is polled by one observer.
 */
export interface DeviceBarContext {
  slavePosition: number
  productName: string
  isCia402: boolean
  alState: number | undefined
  /** PRE-OP or above: the drive can answer an SDO, so the status can be read. */
  mailboxActive: boolean
  status: Cia402Status | undefined
  statusError: unknown
  /** OP: the drive's state machine advances, so a command or a target reaches it. */
  canCommand: boolean
  /** Why canCommand is false, for the tooltip of every disabled control. */
  whyDisabled: string
}

interface DeviceBarProps {
  slavePosition: number
}

/**
 * The bar that stays pinned to the top of every device page.
 *
 * It answers two questions that a long page otherwise loses: **which device am I working on** — the
 * sidebar scrolls away, and with a dozen devices its highlighted entry may be nowhere near the
 * viewport — and **what is this drive doing right now**. For a CiA402 drive it also carries the
 * commands worth having within reach at all times rather than only on the Motion page.
 *
 * The bar is rows. DeviceStatusRow is the first: identity, CiA402 state, mode, the two words, and
 * the mode, state, quick stop and fault reset controls. The bar owns the queries every row reads
 * and passes one DeviceBarContext down, so a second row costs no second poll.
 */
export default function DeviceBar({ slavePosition }: DeviceBarProps) {
  const { api } = useConnection()

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
  const deviceState = statesQuery.data?.data.find((s) => s.slavePosition === slavePosition)
  const alState = deviceState?.alState
  const isCia402 = device?.isCia402 ?? false
  const mailboxActive = alState !== undefined && MAILBOX_ACTIVE_STATES.has(alState)

  const statusQuery = useCia402Status(slavePosition, isCia402 && mailboxActive)

  // Commanding needs OP, for the reason on OP_STATE. Disabled rather than hidden: a button that
  // vanishes leaves you wondering where it went, where a disabled one with a reason tells you what
  // to fix.
  const canCommand = isCia402 && alState === OP_STATE
  const whyDisabled = !isCia402
    ? 'this device is not a CiA402 drive'
    : alState === undefined
      ? 'the device’s EtherCAT state is not known yet'
      : deviceState?.reachability === 'addressLost'
        ? 'the device lost its station address, so it cannot be commanded until a scan'
        : deviceState?.reachability === 'notAnswering'
          ? 'nothing answers at the device’s address or at its wire position'
          : 'the device is not in the OP EtherCAT state, so its CiA402 state machine is not advancing'

  const context: DeviceBarContext = {
    slavePosition,
    productName: device?.productName ?? device?.name ?? `Device ${slavePosition}`,
    isCia402,
    alState,
    mailboxActive,
    status: statusQuery.data,
    statusError: statusQuery.isError ? statusQuery.error : undefined,
    canCommand,
    whyDisabled,
  }

  return (
    <div className="sticky top-0 z-20 border-b border-grey-200 bg-white">
      <DeviceStatusRow context={context} />
    </div>
  )
}
