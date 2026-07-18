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
      <div className="mb-2 flex flex-wrap items-center gap-x-8 gap-y-1">
        <p className="eyebrow">Device {slavePosition}</p>
        {device && (
          <dl className="flex flex-wrap items-baseline gap-x-4 gap-y-1">
            {(
              [
                { label: 'Name', value: device.name, mono: false },
                { label: 'Vendor ID', value: hex32(device.vendorId), mono: true },
                { label: 'Product Code', value: hex32(device.productCode), mono: true, title: device.productName },
                { label: 'Revision', value: hex32(device.revisionNumber), mono: true },
                { label: 'Serial', value: String(device.serialNumber), mono: false },
              ] as { label: string; value: string; mono: boolean; title?: string }[]
            ).map(({ label, value, mono, title }, i) => (
              <div
                key={label}
                title={title}
                className={`flex items-baseline gap-1.5 ${i > 0 ? 'border-l border-grey-200 pl-4' : ''} ${title ? 'cursor-help' : ''}`}
              >
                <dt className="text-[9px] text-grey-500 uppercase tracking-wide font-display">
                  {label}
                </dt>
                <dd className={`text-[10px] text-grey-900 ${mono ? 'font-mono' : ''}`}>{value}</dd>
              </div>
            ))}
          </dl>
        )}
      </div>
      <h1 className="font-display text-4xl font-light">{title}</h1>
      {description && <p className="text-sm text-grey-600 mt-2">{description}</p>}
    </div>
  )
}
