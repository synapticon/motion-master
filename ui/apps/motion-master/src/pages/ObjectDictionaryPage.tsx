import { useParams } from 'react-router'

export default function ObjectDictionaryPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <h1 className="text-2xl font-semibold mb-6">Object Dictionary — Device {deviceId}</h1>
    </div>
  )
}
