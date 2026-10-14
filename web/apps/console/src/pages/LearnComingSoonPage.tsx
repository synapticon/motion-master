import PageHeader from '../components/PageHeader'
import Callout from '../components/Callout'

// One component serves every Learn topic that has a sidebar entry and no text yet, so a topic
// becomes a real page by replacing its route, not by deleting a stub.
export default function LearnComingSoonPage({ title, topic }: { title: string; topic: string }) {
  return (
    <div>
      <PageHeader eyebrow="Learn" title={title} />
      <div className="p-4 sm:px-8 sm:py-7">
        <Callout variant="info" className="max-w-3xl">
          Coming soon. This page will explain {topic}. The text is not written yet.
        </Callout>
      </div>
    </div>
  )
}
