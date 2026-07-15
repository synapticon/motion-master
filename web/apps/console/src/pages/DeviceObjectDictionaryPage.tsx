import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function DeviceObjectDictionaryPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="Object Dictionary" />
    </div>
  )
}
