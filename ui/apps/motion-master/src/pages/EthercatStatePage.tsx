import { useParams } from 'react-router'
import DevicePageHeader from '../components/DevicePageHeader'

export default function EthercatStatePage() {
  const { deviceId } = useParams()
  return (
    <div>
      <DevicePageHeader slavePosition={Number(deviceId)} title="EtherCAT State" />
    </div>
  )
}
