import { useParams } from 'react-router'

export default function EthercatStatePage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">EtherCAT State — Device {deviceId}</h1>
    </div>
  )
}
