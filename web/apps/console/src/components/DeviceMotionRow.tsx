import { useCallback, useEffect, useRef, useState } from 'react'
import { useQuery, useQueryClient } from '@tanstack/react-query'
import { apiErrorMessage, type Cia402Status } from '@synapticon/motion-master-client'
import type { DeviceBarContext } from './DeviceBar'
import { barBtnOutline, barBtnPrimary, labelCls } from './deviceBarStyles'
import { useConnection } from '../contexts/ConnectionContext'
import { cia402StatusKey } from '../hooks/useCia402Status'

// The three cyclic synchronous modes (0x6061), the only modes this row shows for. A profile mode
// has a profile run (#37); homing and the vendor modes have no linear target to drag.
const CSP = 8
const CSV = 9
const CST = 10

type Quantity = 'position' | 'velocity' | 'torque'

// What the row controls in one mode: which target it writes, which status field it follows, and
// where its bounds come from. CST and CSV share one shape, a slider centred on 0 and bounded by a
// single limit object. CSP has no slider: a position target has an absolute scale, and its range
// is the software position limit, which is often unset.
interface ModeControl {
  quantity: Quantity
  label: string
  target: (s: Cia402Status) => number | null
  slider: boolean
}

const MODE_CONTROL: Record<number, ModeControl> = {
  [CST]: {
    quantity: 'torque',
    label: 'Target torque (0x6071)',
    target: (s) => s.targetTorque,
    slider: true,
  },
  [CSV]: {
    quantity: 'velocity',
    label: 'Target velocity (0x60FF)',
    target: (s) => s.targetVelocity,
    slider: true,
  },
  [CSP]: {
    quantity: 'position',
    label: 'Target position (0x607A)',
    target: (s) => s.targetPosition,
    slider: false,
  },
}

// The limit object of each mode. CST and CSV have one, which bounds the slider at ± its value.
// CSP has the two subindices of the software position limit.
const LIMIT_OBJECTS: Record<Quantity, { index: number; subindex: number; name: string }[]> = {
  torque: [{ index: 0x6072, subindex: 0, name: 'Max torque (0x6072)' }],
  velocity: [{ index: 0x6080, subindex: 0, name: 'Max motor speed (0x6080)' }],
  position: [
    { index: 0x607d, subindex: 1, name: 'Software position limit, min (0x607D:01)' },
    { index: 0x607d, subindex: 2, name: 'Software position limit, max (0x607D:02)' },
  ],
}

// The floor between two target writes while the slider is dragged. Writes are sent one at a time,
// the next when the previous one has answered, and never closer than this. A target write in OP
// answers in a few milliseconds, so without the floor a drag would send a hundred writes a second.
// A release ignores the floor: 0 goes out as soon as the line is free.
const DRAG_SEND_INTERVAL_MS = 100

const INT32_MIN = -2147483648
const INT32_MAX = 2147483647

const limitKey = (slavePosition: number, index: number, subindex: number) => [
  'motionLimit',
  slavePosition,
  index,
  subindex,
]

// One limit object, read once and kept until Sync limits asks again. The value is read live, PDO
// when exchanging else SDO, so it is the drive's current word on the matter at the time of the
// read. A failed read, a missing object and a non-numeric value all leave the limit unknown.
function useLimit(slavePosition: number, index: number, subindex: number, enabled: boolean) {
  const { api } = useConnection()
  return useQuery({
    queryKey: limitKey(slavePosition, index, subindex),
    queryFn: () => api.readParameter(slavePosition, index, subindex).then((r) => r.data.value),
    enabled,
    staleTime: Infinity,
    retry: false,
  })
}

interface DeviceMotionRowProps {
  context: DeviceBarContext
}

/**
 * The second row of DeviceBar: the three actual values and a control for the target of the active
 * cyclic synchronous mode, so an axis can be moved by hand from any device page.
 *
 * Every send is one raw write of the target object, which the drive follows at once in a cyclic
 * synchronous mode. The slider is a throttle: it sends while it is dragged and springs back to 0
 * on release, which is sent too, so letting go stops the command. Set and Go each send one
 * value, and the drive's own limits shape the step. #54 adds a smoothed target; the row switches
 * to it then.
 *
 * Writes go out one at a time through a latest-wins slot, the same rule the server's mailbox
 * follows: a newer value replaces one that has not left yet, nothing queues, and a release's 0 is
 * sent after the write in flight, so no drag value can land after it.
 */
export default function DeviceMotionRow({ context }: DeviceMotionRowProps) {
  const { slavePosition, isCia402, mailboxActive, status, canCommand, whyDisabled } = context
  const { api } = useConnection()
  const queryClient = useQueryClient()

  const control = status ? MODE_CONTROL[status.modeOfOperation] : undefined
  const show = isCia402 && status !== undefined && control !== undefined
  const quantity = control?.quantity ?? 'position'
  const limitObjects = LIMIT_OBJECTS[quantity]

  // Every mode's limit hook is called every render, because hooks cannot be conditional. Only the
  // active mode's queries are enabled; the other two sit idle at no cost.
  const torqueLimit = useLimit(slavePosition, 0x6072, 0, show && mailboxActive && quantity === 'torque')
  const velocityLimit = useLimit(
    slavePosition,
    0x6080,
    0,
    show && mailboxActive && quantity === 'velocity',
  )
  const positionMin = useLimit(
    slavePosition,
    0x607d,
    1,
    show && mailboxActive && quantity === 'position',
  )
  const positionMax = useLimit(
    slavePosition,
    0x607d,
    2,
    show && mailboxActive && quantity === 'position',
  )

  const syncLimits = () =>
    queryClient.refetchQueries({ queryKey: ['motionLimit', slavePosition] })
  const limitsPending =
    torqueLimit.isFetching || velocityLimit.isFetching || positionMin.isFetching || positionMax.isFetching

  const drivesTarget = status ? control?.target(status) ?? null : null

  // The input follows the drive's target until it has focus or holds an edit that was not sent.
  const [inputText, setInputText] = useState('')
  const [inputFocused, setInputFocused] = useState(false)
  const [inputEdited, setInputEdited] = useState(false)
  const [inputError, setInputError] = useState<string | null>(null)
  useEffect(() => {
    if (!inputFocused && !inputEdited) {
      setInputText(drivesTarget === null ? '' : String(drivesTarget))
    }
  }, [drivesTarget, inputFocused, inputEdited])

  // The one send slot. `intended` is the newest value not yet sent, or null; `inFlight` says a
  // request is out. `pump` sends the intended value when the line is free and the floor has
  // passed, and runs again when the answer comes back. A send from a button patches the status
  // snapshot with its value on success, because the write answers with nothing and the input
  // would otherwise fall back to the old target for up to one poll.
  const intended = useRef<{ value: number; patch: boolean } | null>(null)
  const inFlight = useRef(false)
  const lastSentAt = useRef(0)
  const floorTimer = useRef<ReturnType<typeof setTimeout> | null>(null)
  const [sendError, setSendError] = useState<string | null>(null)

  const pump = useCallback(() => {
    if (inFlight.current || intended.current === null) {
      return
    }
    const wait = lastSentAt.current + DRAG_SEND_INTERVAL_MS - Date.now()
    if (wait > 0) {
      if (floorTimer.current === null) {
        floorTimer.current = setTimeout(() => {
          floorTimer.current = null
          pump()
        }, wait)
      }
      return
    }
    const { value, patch } = intended.current
    intended.current = null
    inFlight.current = true
    lastSentAt.current = Date.now()
    api
      .setCia402Target(slavePosition, { target: quantity, value })
      .then(() => {
        setSendError(null)
        if (patch) {
          queryClient.setQueryData(
            cia402StatusKey(slavePosition),
            (old: Cia402Status | undefined) => {
              if (!old) {
                return old
              }
              const field: Record<Quantity, keyof Cia402Status> = {
                position: 'targetPosition',
                velocity: 'targetVelocity',
                torque: 'targetTorque',
              }
              return { ...old, [field[quantity]]: value }
            },
          )
          void queryClient.invalidateQueries({ queryKey: cia402StatusKey(slavePosition) })
          setInputEdited(false)
          setInputError(null)
        }
      })
      .catch((e: unknown) => setSendError(apiErrorMessage(e)))
      .finally(() => {
        inFlight.current = false
        pump()
      })
  }, [api, slavePosition, quantity, queryClient])

  // A button's value: sent once, and the input follows it afterwards.
  const send = (value: number) => {
    intended.current = { value, patch: true }
    lastSentAt.current = 0
    pump()
  }

  // The throttle. `drag` is the handle while it is held; null means it rests at 0.
  const [drag, setDrag] = useState<number | null>(null)
  const dragging = useRef(false)
  const dragTo = (value: number) => {
    dragging.current = true
    setDrag(value)
    intended.current = { value, patch: false }
    pump()
  }
  const release = useCallback(() => {
    if (!dragging.current) {
      return
    }
    dragging.current = false
    setDrag(null)
    intended.current = { value: 0, patch: true }
    lastSentAt.current = 0
    pump()
  }, [pump])

  // Letting go must stop the command whatever took the pointer away: a cancelled pointer, the
  // window losing focus, the tab being hidden, or this row going away mid-drag. A browser that
  // dies keeps nothing running here, but the drive keeps the last target it was sent.
  useEffect(() => {
    const onHidden = () => {
      if (document.visibilityState === 'hidden') {
        release()
      }
    }
    window.addEventListener('blur', release)
    document.addEventListener('visibilitychange', onHidden)
    return () => {
      window.removeEventListener('blur', release)
      document.removeEventListener('visibilitychange', onHidden)
      release()
      if (floorTimer.current !== null) {
        clearTimeout(floorTimer.current)
        floorTimer.current = null
      }
    }
  }, [release])

  // A target reaches the drive only in OP and only while the drive is enabled; in any other CiA402
  // state the drive ignores the object until it is enabled, and then follows it in one step.
  const enabled = canCommand && status?.state === 'OperationEnabled'
  const whyNot = !canCommand
    ? whyDisabled
    : `the drive is in ${status?.state ?? 'an unknown state'}, not Operation Enabled`

  // Here copies the actual value into the target on the server, for the kind of the active mode.
  // It is the explicit step before an enable in CSP: the drive holds its actual position only until
  // the first setpoint reaches it, and in CSP that is 0x607A as it stands. It needs OP, so the write
  // reaches the drive, and nothing more: its whole point is to run before Operation Enabled.
  const [hereError, setHereError] = useState<string | null>(null)
  const [herePending, setHerePending] = useState(false)
  const here = () => {
    setHerePending(true)
    api
      .setCia402TargetToActual(slavePosition, { kind: quantity })
      .then((r) => {
        setHereError(null)
        queryClient.setQueryData(cia402StatusKey(slavePosition), (old: Cia402Status | undefined) => {
          if (!old) {
            return old
          }
          const field: Record<Quantity, keyof Cia402Status> = {
            position: 'targetPosition',
            velocity: 'targetVelocity',
            torque: 'targetTorque',
          }
          return { ...old, [field[quantity]]: r.data.value }
        })
        void queryClient.invalidateQueries({ queryKey: cia402StatusKey(slavePosition) })
        setInputEdited(false)
        setInputError(null)
      })
      .catch((e: unknown) => setHereError(apiErrorMessage(e)))
      .finally(() => setHerePending(false))
  }
  // The case Here exists for, called out while it is still avoidable: CSP, not enabled, and a
  // target position that is not the actual one. Shown for a drive in OP, where the note is a step
  // away from mattering, and not elsewhere, where the row already says why nothing can be sent.
  const staleTarget =
    quantity === 'position' &&
    canCommand &&
    status?.state !== 'OperationEnabled' &&
    status?.targetPosition !== null &&
    status?.actualPosition !== null &&
    status?.targetPosition !== status?.actualPosition

  // Bounds. CST and CSV: ± the limit object, when it was read and is not 0. CSP: the software
  // position limit when both ends were read and differ, else the whole INTEGER32 range.
  const numeric = (v: unknown): number | null => (typeof v === 'number' ? v : null)
  let min = INT32_MIN
  let max = INT32_MAX
  let sliderDisabledReason: string | null = null
  let boundsNote: string
  if (quantity === 'torque' || quantity === 'velocity') {
    const q = quantity === 'torque' ? torqueLimit : velocityLimit
    const obj = limitObjects[0]
    const limit = numeric(q.data)
    if (q.isError) {
      sliderDisabledReason = `${obj.name} could not be read: ${apiErrorMessage(q.error)}`
    } else if (q.isPending) {
      sliderDisabledReason = `${obj.name} is being read`
    } else if (limit === null) {
      sliderDisabledReason = `${obj.name} is not a number on this drive`
    } else if (limit === 0) {
      sliderDisabledReason = `${obj.name} reads 0, so there is no range to slide over`
    } else {
      min = -Math.abs(limit)
      max = Math.abs(limit)
    }
    boundsNote =
      sliderDisabledReason === null
        ? `Bounded by ± ${obj.name}: ${min} to ${max}.`
        : `Unbounded, because ${sliderDisabledReason}.`
  } else {
    const lo = numeric(positionMin.data)
    const hi = numeric(positionMax.data)
    if (lo !== null && hi !== null && lo !== hi) {
      min = Math.min(lo, hi)
      max = Math.max(lo, hi)
      boundsNote = `Bounded by the software position limit (0x607D): ${min} to ${max}.`
    } else {
      boundsNote = `Bounded by the INTEGER32 range, because the software position limit (0x607D) is ${
        lo === null || hi === null ? 'not readable on this drive' : 'not set'
      }.`
    }
  }

  const parseInput = (): number | null => {
    const text = inputText.trim()
    if (text === '' || !/^-?\d+$/.test(text)) {
      setInputError('enter a whole number')
      return null
    }
    const value = Number(text)
    if (value < min || value > max) {
      setInputError(`${value} is outside ${min} to ${max}`)
      return null
    }
    setInputError(null)
    return value
  }

  const sendInput = () => {
    const value = parseInput()
    if (value !== null) {
      send(value)
    }
  }

  if (!show) {
    return null
  }

  const handle = drag ?? 0
  const sliderLive = enabled && sliderDisabledReason === null
  const disabledTitle = (name: string) => `${name} — unavailable: ${whyNot}.`

  const actual = (label: string, value: number | null) => (
    <div className="flex items-baseline gap-1.5">
      <span className={labelCls}>{label}</span>
      <span className="inline-block w-[12ch] font-mono text-xs tabular-nums">
        {value === null ? '—' : value}
      </span>
    </div>
  )

  return (
    <div className="flex flex-wrap items-center justify-between gap-x-6 gap-y-2 border-t border-grey-200 px-4 py-2 sm:px-8">
      <div className="flex flex-wrap items-center gap-x-4 gap-y-2">
        <span className={labelCls}>Motion</span>
        <div className="flex flex-wrap items-center gap-x-4 gap-y-1 border-l border-grey-200 pl-4">
          {actual('Position (0x6064)', status.actualPosition)}
          {actual('Velocity (0x606C)', status.actualVelocity)}
          {actual('Torque (0x6077)', status.actualTorque)}
        </div>
      </div>

      <div className="flex flex-wrap items-center gap-2">
        <span className={labelCls}>{control.label}</span>

        {control.slider && (
          <div className="flex items-center gap-2">
            <span className="font-mono text-[10px] tabular-nums text-grey-500">{min}</span>
            <input
              type="range"
              aria-label={control.label}
              className="throttle h-1 w-[480px] cursor-pointer appearance-none bg-grey-200 disabled:cursor-not-allowed"
              min={min}
              max={max}
              step={1}
              value={handle}
              disabled={!sliderLive}
              onChange={(e) => dragTo(Number(e.target.value))}
              onPointerUp={release}
              onPointerCancel={release}
              onLostPointerCapture={release}
              onKeyUp={release}
              onBlur={release}
              title={
                !enabled
                  ? disabledTitle('Throttle')
                  : sliderDisabledReason !== null
                    ? `Throttle — unavailable: ${sliderDisabledReason}. The input and Set still work.`
                    : `Throttle — hold and drag to command a ${quantity} target; the value is sent while you drag, at most every ${DRAG_SEND_INTERVAL_MS} ms. Letting go sends 0 and the handle springs back. ${boundsNote}`
              }
            />
            <span className="font-mono text-[10px] tabular-nums text-grey-500">{max}</span>
          </div>
        )}

        <input
          type="number"
          aria-label={`${control.label} value`}
          className="h-[30px] w-32 border border-grey-300 px-2 font-mono text-xs disabled:cursor-not-allowed disabled:opacity-50"
          min={min}
          max={max}
          step={1}
          value={inputText}
          disabled={!enabled}
          onFocus={() => setInputFocused(true)}
          onBlur={() => setInputFocused(false)}
          onChange={(e) => {
            setInputText(e.target.value)
            setInputEdited(true)
            setInputError(null)
          }}
          onKeyDown={(e) => {
            if (e.key === 'Enter') {
              sendInput()
            }
          }}
          title={
            enabled
              ? `${control.label} — the value to send with ${control.slider ? 'Set' : 'Go'}, in the object's raw units. It follows the drive's target until you edit it. ${boundsNote}`
              : disabledTitle(control.label)
          }
        />

        {control.slider ? (
          <button
            type="button"
            className={barBtnOutline}
            disabled={!enabled}
            onClick={sendInput}
            title={
              enabled
                ? `Set — send the value in the input as the ${quantity} target, once. The drive steps to it, within its own limits, and holds it until the next send.`
                : disabledTitle('Set')
            }
          >
            Set
          </button>
        ) : (
          <>
            <button
              type="button"
              className={barBtnPrimary}
              disabled={!enabled}
              onClick={sendInput}
              title={
                enabled
                  ? 'Go — send the typed position as the target, once. The drive steps to it as fast as its limits allow; there is no profile. Check the distance before pressing.'
                  : disabledTitle('Go')
              }
            >
              Go
            </button>
            <button
              type="button"
              className={staleTarget ? barBtnPrimary : barBtnOutline}
              disabled={!canCommand || herePending}
              onClick={here}
              title={
                canCommand
                  ? 'Here — set the target position (0x607A) to the actual position (0x6064), on the server, once. Do this before you enable the drive in CSP: a drive enabled with a stale target steps to it at once, and that step can pull the supply down. While enabled it holds the axis where it is.'
                  : `Here — unavailable: ${whyDisabled}.`
              }
            >
              Here
            </button>
          </>
        )}

        <button
          type="button"
          className={barBtnOutline}
          disabled={!mailboxActive || limitsPending}
          onClick={() => void syncLimits()}
          title={
            mailboxActive
              ? `Sync limits — read ${limitObjects.map((o) => o.name).join(' and ')} from the drive again. The bounds are read once when the row first shows and after a change to the objects only on this button.`
              : 'Sync limits — unavailable: the device has no mailbox in this EtherCAT state.'
          }
        >
          Sync limits
        </button>

        {(inputError ?? sendError ?? hereError) && (
          <span
            className="max-w-[28ch] truncate text-xs text-status-bad"
            title={inputError ?? sendError ?? hereError ?? undefined}
          >
            {inputError ?? sendError ?? hereError}
          </span>
        )}
        {staleTarget && (
          <span
            className="rounded-sm bg-status-warn px-2.5 py-0.5 text-xs text-grey-900"
            title="The target position (0x607A) is not the actual position (0x6064). Enabling now makes the drive step to the target at once. Press Here first."
          >
            Target differs from actual — press Here before enabling
          </span>
        )}
      </div>
    </div>
  )
}
