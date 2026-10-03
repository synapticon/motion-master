import { useQuery } from '@tanstack/react-query'
import { useConnection } from '../contexts/ConnectionContext'

/**
 * Whether the devices have an EtherCAT Slave Controller: SII, ESC registers, distributed clocks
 * and the ESC diagnostics. A SPoE connection has none, and the server answers 409 on those
 * endpoints.
 *
 * With @p slavePosition it answers for that device, otherwise for the whole bus. It answers true
 * until the device list is known, so a page does not flash a notice while it loads.
 */
export function useSupportsEsc(slavePosition?: number): boolean {
  const { api, hasScanned } = useConnection()
  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    enabled: hasScanned,
  })
  const devices = devicesQuery.data?.data ?? []
  if (slavePosition !== undefined) {
    return devices.find(d => d.slavePosition === slavePosition)?.supportsEsc !== false
  }
  return !devices.some(d => d.supportsEsc === false)
}
