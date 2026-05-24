import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function SiiPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="SII" />
    </div>
  )
}
