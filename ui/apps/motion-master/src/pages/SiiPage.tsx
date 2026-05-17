import { useParams } from 'react-router'

export default function SiiPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">SII — Device {deviceId}</h1>
    </div>
  )
}
