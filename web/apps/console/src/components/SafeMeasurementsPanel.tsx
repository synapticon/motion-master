import type { FsoeConnection } from '@synapticon/motion-master-client'
import Section from './Section'
import SafeProcessValuesBody, { Badge } from './SafeProcessValuesPanel'
import SafeSensorBody from './SafeSensorPanel'

/**
 * The safe measuring channel: what it currently reads, and the configuration
 * that decides whether those readings are trusted.
 *
 * These were two sections. They are one subject. The tolerances, discrepancy
 * timers and high-water marks in the lower half exist solely to judge the values
 * in the upper half, so reading a velocity and then scrolling to the window that
 * validates it meant holding a number in your head to compare it with its own
 * limit.
 *
 * The halves have different requirements, which is why they stay separate
 * bodies: the live values need an FSoE connection, and the configuration reads
 * over SDO and is available without one - which is exactly when it is most
 * wanted, because a connection that will not open is usually a connection whose
 * safety parameters are wrong.
 */
export default function SafeMeasurementsPanel({
  slavePosition,
  connection,
}: {
  slavePosition: number
  connection: FsoeConnection | undefined
}) {
  const values = connection?.processValues

  return (
    <Section
      title="Safe measurements"
      chips={
        connection ? (
          <>
            <Badge
              ok={values?.crossCheckOk ?? false}
              label="cross-check"
              title="Two sensor channels are configured and agree."
            />
            <Badge
              ok={values?.positionReferenced ?? false}
              label="referenced"
              title="The safe position has an established absolute origin. Anything applying an absolute limit — Safe Limited Position above all — must gate on this, not on the position's validity flag."
            />
            {!connection.inputsValid && (
              <span className="text-xs text-status-warn">
                fail-safe: these are not live measurements
              </span>
            )}
          </>
        ) : (
          <span className="text-xs text-grey-500">no connection — configuration only</span>
        )
      }
    >
      {connection ? (
        <SafeProcessValuesBody connection={connection} />
      ) : (
        <p className="p-4 text-sm text-grey-500">
          There is no FSoE connection, so the drive is publishing no safe values. The configuration
          below reads over SDO and is available anyway.
        </p>
      )}
      <SafeSensorBody slavePosition={slavePosition} />
    </Section>
  )
}
