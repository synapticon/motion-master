import { useParams } from 'react-router'
import PageHeader from '../components/PageHeader'

export default function SiiPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <PageHeader eyebrow={`Device ${deviceId}`} title="SII" />
    </div>
  )
}
