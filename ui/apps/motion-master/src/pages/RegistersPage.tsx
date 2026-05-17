import { useParams } from 'react-router'

export default function RegistersPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">Registers — Device {deviceId}</h1>
    </div>
  )
}
