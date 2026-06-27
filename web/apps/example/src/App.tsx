import { useState } from 'react'
import { Routes, Route, Link, useLocation } from 'react-router'
import { Api, API_BASE_URL, formatHex } from '@synapticon/motion-master-client'
import { PageHeader, Callout, Button } from '@synapticon/ui'

// A shared library used offline: the client ships pure, framework-agnostic helpers
// (no server needed) alongside the HTTP/WebSocket client.
const DEMO_VALUE = 0x80a0

// The same client the real apps use, pointed at a local Motion Master.
const api = new Api({ baseUrl: API_BASE_URL })

function ServerCheck() {
  const [state, setState] = useState<
    { status: 'idle' | 'loading' } | { status: 'ok'; version: string } | { status: 'err'; message: string }
  >({ status: 'idle' })

  async function check() {
    setState({ status: 'loading' })
    try {
      const res = await api.getVersion()
      setState({ status: 'ok', version: res.data.version })
    } catch (e) {
      setState({ status: 'err', message: e instanceof Error ? e.message : String(e) })
    }
  }

  return (
    <div className="flex flex-col gap-3">
      <Button onClick={check} disabled={state.status === 'loading'}>
        {state.status === 'loading' ? 'Checking…' : 'Check server'}
      </Button>
      {state.status === 'ok' && (
        <Callout variant="info">
          Connected — Motion Master reports version <code>{state.version}</code>.
        </Callout>
      )}
      {state.status === 'err' && (
        <Callout variant="warning">
          No server reachable at <code>{API_BASE_URL}</code>. That's expected unless a local Motion
          Master is running. ({state.message})
        </Callout>
      )}
    </div>
  )
}

function Home() {
  return (
    <div className="flex flex-col gap-6 px-8 py-7">
      <p className="text-sm text-grey-600">
        This is a minimal starter app. Every visible piece comes from a shared workspace package, so
        a new app can copy this directory and start with the design system and client already wired.
      </p>

      <section className="flex flex-col gap-2">
        <p className="eyebrow">Shared design system</p>
        <p className="text-sm text-grey-600">
          The theme, fonts, and base styles are imported from <code>@synapticon/ui/theme.css</code>.
          Headings, the red eyebrow above, and the squared buttons all come from it.
        </p>
      </section>

      <section className="flex flex-col gap-2">
        <p className="eyebrow">Shared components</p>
        <p className="text-sm text-grey-600">
          <code>PageHeader</code>, <code>Callout</code>, and <code>Button</code> below are imported
          from <code>@synapticon/ui</code>.
        </p>
        <Callout variant="info">A shared Callout, rendered with the shared theme.</Callout>
      </section>

      <section className="flex flex-col gap-2">
        <p className="eyebrow">Shared client library</p>
        <p className="text-sm text-grey-600">
          A pure helper from <code>@synapticon/motion-master-client</code> works with no server:{' '}
          <code>formatHex(0x80a0)</code> → <code>{formatHex(DEMO_VALUE)}</code>. The same package
          also provides the HTTP client:
        </p>
        <ServerCheck />
      </section>

      <section className="flex flex-col gap-2">
        <p className="eyebrow">Routing</p>
        <p className="text-sm text-grey-600">
          Client-side routing works under <code>/apps/example</code>, and cold-loaded deep links
          survive via the 404 redirect + decoder.{' '}
          <Link className="text-ocean hover:text-grey-900" to="/about">
            Go to /about →
          </Link>
        </p>
      </section>
    </div>
  )
}

function About() {
  const { pathname } = useLocation()
  return (
    <div className="flex flex-col gap-4 px-8 py-7">
      <p className="text-sm text-grey-600">
        You're at <code>{pathname}</code>. Reload this page — the deep link is restored by the
        decoder snippet in <code>index.html</code>, so it keeps working on a cold load.
      </p>
      <Link className="text-ocean hover:text-grey-900 text-sm" to="/">
        ← Back home
      </Link>
    </div>
  )
}

export default function App() {
  return (
    <div className="min-h-screen">
      <PageHeader
        eyebrow="Motion Master"
        title="Example App"
        description="A starter that shares the Synapticon UI and client library."
      />
      <Routes>
        <Route path="/" element={<Home />} />
        <Route path="/about" element={<About />} />
      </Routes>
    </div>
  )
}
