import { useParams } from 'react-router'

export default function FoePage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">FoE — Device {deviceId}</h1>
    </div>
  )
}
