import { useParams } from 'react-router'
import PageHeader from '../components/PageHeader'

export default function ObjectDictionaryPage() {
  const { deviceId } = useParams()
  return (
    <div>
      <PageHeader eyebrow={`Device ${deviceId}`} title="Object Dictionary" />
    </div>
  )
}
