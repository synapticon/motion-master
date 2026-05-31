import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

export default function ConnectionPage() {
  const { host, port, setHost, setPort } = useConnection()

  return (
    <div>
      <PageHeader
        eyebrow="App"
        title="Connection"
        description="Configure the host and port used to reach the Motion Master backend."
      />
      <div className="p-4 sm:p-8 space-y-8">
        <section>
          <div className="border border-grey-200 p-5">
            <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
              <div>
                <label className={labelCls}>Host</label>
                <input
                  type="text"
                  value={host}
                  onChange={e => setHost(e.target.value)}
                  placeholder="local.motion-master.synapticon.com"
                  className={inputCls}
                />
              </div>
              <div>
                <label className={labelCls}>Port</label>
                <input
                  type="text"
                  value={port}
                  onChange={e => setPort(e.target.value)}
                  placeholder="8443"
                  className={inputCls}
                />
              </div>
            </div>
          </div>
        </section>
      </div>
    </div>
  )
}
