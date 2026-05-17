import { useParams } from 'react-router'
import PageHeader from '../components/PageHeader'

export default function ProcessDataPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <PageHeader eyebrow={`Device ${deviceId}`} title="Process Data" />
    </div>
  )
}
