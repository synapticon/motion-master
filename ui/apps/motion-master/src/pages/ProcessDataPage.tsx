import { useParams } from 'react-router'

export default function ProcessDataPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">Process Data — Device {deviceId}</h1>
    </div>
  )
}
