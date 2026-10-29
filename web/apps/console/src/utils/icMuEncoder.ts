import type { EncoderConfiguration } from '@synapticon/motion-master-client'

// The encoders the status procedure lists. Ports 3 to 5 take an external encoder, which the status
// reads as an iC-MU only when register 0x74 names an iC-MU part. The server decides with the same rule
// (somanet::isIcMuEncoder); the page uses it only to know which cards to draw before a status read.
export function isIcMuEncoder(encoder: EncoderConfiguration): boolean {
  return (
    encoder.sensorPort >= 1 &&
    encoder.sensorPort <= 5 &&
    (encoder.type === 'biss' || encoder.type === 'internal') &&
    encoder.singleturnBits !== 0
  )
}
