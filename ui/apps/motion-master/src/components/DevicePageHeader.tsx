import { useQuery } from '@tanstack/react-query'
import type { ReactNode } from 'react'
import { useConnection } from '../contexts/ConnectionContext'

interface DevicePageHeaderProps {
  slavePosition: number
  title: string
  description?: ReactNode
}

function hex32(n: number) {
  return `0x${n.toString(16).toUpperCase().padStart(8, '0')}`
}

export default function DevicePageHeader({
  slavePosition,
  title,
  description,
}: DevicePageHeaderProps) {
  const { api } = useConnection()

  const devicesQuery = useQuery({
    queryKey: ['devices'],
    queryFn: () => api.getDevices(),
    staleTime: Infinity,
  })

  const device = devicesQuery.data?.data.find(d => d.slavePosition === slavePosition)

  return (
    <div className="px-8 py-7 border-b border-grey-200">
      <div className="flex flex-wrap items-start justify-between gap-x-8 gap-y-4">
        <div>
          <p className="eyebrow mb-2">Device {slavePosition}</p>
          <h1 className="font-display text-4xl font-light">{title}</h1>
        </div>
        {device && (
          <dl className="flex flex-wrap gap-x-6 gap-y-1 border border-grey-200 px-4 py-3">
            {[
              { label: 'Name', value: device.name, mono: false },
              { label: 'Vendor ID', value: hex32(device.vendorId), mono: true },
              { label: 'Product Code', value: hex32(device.productCode), mono: true },
              { label: 'Revision', value: hex32(device.revisionNumber), mono: true },
              { label: 'Serial', value: String(device.serialNumber), mono: false },
            ].map(({ label, value, mono }) => (
              <div key={label}>
                <dt className="text-xs text-grey-500 uppercase tracking-wide">{label}</dt>
                <dd className={`text-xs text-grey-900 ${mono ? 'font-mono' : ''}`}>{value}</dd>
              </div>
            ))}
          </dl>
        )}
      </div>
      {description && <p className="text-sm text-grey-600 mt-2">{description}</p>}
    </div>
  )
}
