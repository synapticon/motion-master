import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function ObjectDictionaryPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="Object Dictionary" />
    </div>
  )
}
