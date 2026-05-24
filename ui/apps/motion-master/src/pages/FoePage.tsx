import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function FoePage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="FoE" />
    </div>
  )
}
