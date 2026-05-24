import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function ProcessDataPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="Process Data" />
    </div>
  )
}
