import Callout from './Callout'

/**
 * Says that a feature needs an EtherCAT Slave Controller, which a SPoE connection does not have.
 * Shown in place of a page, or a part of one, that would otherwise only show the server's 409.
 */
export default function NoEscNotice({ feature }: { feature: string }) {
  return (
    <Callout variant="info">
      {feature} needs an EtherCAT Slave Controller. The fieldbus is connected over SPoE, which reaches
      each drive over Ethernet without one, so {feature.toLowerCase()} is not available here.
    </Callout>
  )
}
