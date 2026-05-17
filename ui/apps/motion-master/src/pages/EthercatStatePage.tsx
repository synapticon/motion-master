import { useParams } from 'react-router'
import PageHeader from '../components/PageHeader'

export default function EthercatStatePage() {
  const { deviceId } = useParams()
  return (
    <div>
      <PageHeader eyebrow={`Device ${deviceId}`} title="EtherCAT State" />
    </div>
  )
}
