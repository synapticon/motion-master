import { useState } from 'react'
import { useParams } from 'react-router'
import { useQuery } from '@tanstack/react-query'
import DevicePageHeader from '../components/DevicePageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { parseHexBytes } from '@synapticon/motion-master-client'

const inputCls = 'border border-grey-300 px-3 py-2 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'
const btnCls =
  'bg-syn-red text-white px-4 py-2 text-xs hover:bg-ocean disabled:opacity-50 disabled:cursor-not-allowed cursor-pointer transition-colors'

function apiError(err: unknown): string {
  if (err && typeof err === 'object' && 'error' in err) {
    const inner = (err as { error: unknown }).error
    if (inner && typeof inner === 'object' && 'error' in inner) {
      return String((inner as { error: unknown }).error)
    }
  }
  return 'Unknown error'
}

function interpretBytes(bytes: number[]): string | null {
  const view = new DataView(new Uint8Array(bytes).buffer)
  if (bytes.length === 1) return `${view.getUint8(0)} (uint8)`
  if (bytes.length === 2) return `${view.getUint16(0, true)} (uint16 LE)`
  if (bytes.length === 4) return `${view.getUint32(0, true)} (uint32 LE)`
  if (bytes.length === 8) return `${view.getBigUint64(0, true).toString()} (uint64 LE)`
  return null
}

export default function DeviceRegistersPage() {
  const { deviceId } = useParams()
  const { api } = useConnection()
  const slavePosition = Number(deviceId)

  const catalogueQuery = useQuery({
    queryKey: ['registers'],
    queryFn: () => api.getRegisters(),
    staleTime: Infinity,
  })
  const catalogue = catalogueQuery.data?.data ?? []

  const [address, setAddress] = useState('')
  const [length, setLength] = useState('1')
  const [reading, setReading] = useState(false)
  const [readResult, setReadResult] = useState<number[] | null>(null)
  const [readError, setReadError] = useState<string | null>(null)

  const [writeAddress, setWriteAddress] = useState('')
  const [writeValue, setWriteValue] = useState('')
  const [writing, setWriting] = useState(false)
  const [writeOk, setWriteOk] = useState(false)
  const [writeError, setWriteError] = useState<string | null>(null)

  const addrNum = parseInt(address, 10)
  const addrHex = isNaN(addrNum) ? '—' : `0x${addrNum.toString(16).toUpperCase().padStart(4, '0')}`
  const lenNum = parseInt(length, 10)
  const canRead = !isNaN(addrNum) && addrNum >= 0 && !isNaN(lenNum) && lenNum >= 1 && !reading

  const writeAddrNum = parseInt(writeAddress, 10)
  const writeAddrHex = isNaN(writeAddrNum)
    ? '—'
    : `0x${writeAddrNum.toString(16).toUpperCase().padStart(4, '0')}`
  const writeBytes = parseHexBytes(writeValue)
  const canWrite = !isNaN(writeAddrNum) && writeAddrNum >= 0 && writeBytes !== null && !writing

  function selectFromCatalogue(addr: number) {
    const reg = catalogue.find(r => r.address === addr)
    if (!reg) return
    setAddress(String(reg.address))
    setLength(String(reg.length))
    setReadResult(null)
    setReadError(null)
  }

  function handleAddressChange(val: string) {
    setAddress(val)
    setReadResult(null)
    setReadError(null)
  }

  function handleLengthChange(val: string) {
    setLength(val)
    setReadResult(null)
    setReadError(null)
  }

  async function handleRead() {
    if (!canRead) return
    setReading(true)
    setReadResult(null)
    setReadError(null)
    try {
      const res = await api.readRegister(slavePosition, addrNum, { length: lenNum })
      setReadResult(res.data.data)
    } catch (err) {
      setReadError(apiError(err))
    } finally {
      setReading(false)
    }
  }

  function handleWriteAddressChange(val: string) {
    setWriteAddress(val)
    setWriteOk(false)
    setWriteError(null)
  }

  function handleWriteValueChange(val: string) {
    setWriteValue(val)
    setWriteOk(false)
    setWriteError(null)
  }

  async function handleWrite() {
    if (!canWrite || writeBytes === null) return
    const ok = window.confirm(
      `Write ${writeBytes.length} byte(s) to register ${writeAddrHex} on slave ${slavePosition}?\n` +
        'Writing ESC registers directly can disrupt device operation.',
    )
    if (!ok) return
    setWriting(true)
    setWriteOk(false)
    setWriteError(null)
    try {
      await api.writeRegister(slavePosition, writeAddrNum, { data: writeBytes })
      setWriteOk(true)
    } catch (err) {
      setWriteError(apiError(err))
    } finally {
      setWriting(false)
    }
  }

  const selectedInCatalogue = isNaN(addrNum) ? undefined : catalogue.find(r => r.address === addrNum)

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Registers"
        description={
          <>
            Read and write the device's EtherCAT Slave Controller (ESC) registers directly by
            address — the low-level hardware registers in the slave (AL control/status, Sync Manager
            and FMMU configuration, distributed clocks, error counters), distinct from the CoE
            object dictionary. Pick a known register from the catalogue or enter a raw address.
            Writing arbitrary registers can disrupt communication — use with care.
          </>
        }
      />
      <div className="p-4 sm:p-8 space-y-6">

        <div className="grid grid-cols-1 lg:grid-cols-2 gap-8 items-start">

        {/* Read form */}
        <section>
          <p className="eyebrow mb-5">Read Register</p>
          <div className="border border-grey-200 p-5 space-y-4">

            <div>
              <label className={labelCls}>Known Register</label>
              <select
                className={inputCls}
                value={selectedInCatalogue?.address ?? ''}
                onChange={e => selectFromCatalogue(Number(e.target.value))}
                disabled={catalogueQuery.isPending}
              >
                <option value="">— select a register —</option>
                {catalogue.map(r => (
                  <option key={r.address} value={r.address}>
                    {r.name} · {r.address} / 0x{r.address.toString(16).toUpperCase().padStart(4, '0')} · {r.length}B
                  </option>
                ))}
              </select>
            </div>

            <div className="grid grid-cols-2 gap-3">
              <div>
                <label className={labelCls}>Address (decimal)</label>
                <input
                  type="number"
                  min={0}
                  max={65535}
                  value={address}
                  onChange={e => handleAddressChange(e.target.value)}
                  placeholder="e.g. 272"
                  className={inputCls}
                />
                <p className="text-xs text-grey-500 mt-1 font-mono">{addrHex}</p>
              </div>
              <div>
                <label className={labelCls}>Length (bytes)</label>
                <input
                  type="number"
                  min={1}
                  max={512}
                  value={length}
                  onChange={e => handleLengthChange(e.target.value)}
                  placeholder="e.g. 2"
                  className={inputCls}
                />
              </div>
            </div>

            <button onClick={handleRead} disabled={!canRead} className={btnCls}>
              {reading ? 'Reading…' : 'Read'}
            </button>

            {readError && (
              <p className="text-xs text-status-bad font-mono">{readError}</p>
            )}

            {readResult && (
              <div className="border border-grey-200 p-3 space-y-1 bg-grey-50">
                <p className="text-xs text-grey-600 uppercase tracking-wide mb-2">Result</p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Hex:&nbsp;</span>
                  {readResult.map(b => b.toString(16).toUpperCase().padStart(2, '0')).join(' ')}
                </p>
                <p className="text-xs font-mono">
                  <span className="text-grey-600">Dec:&nbsp;</span>
                  [{readResult.join(', ')}]
                </p>
                {interpretBytes(readResult) && (
                  <p className="text-xs font-mono">
                    <span className="text-grey-600">Value:&nbsp;</span>
                    {interpretBytes(readResult)}
                  </p>
                )}
              </div>
            )}
          </div>
        </section>

        {/* Write form */}
        <section>
          <p className="eyebrow mb-5">Write Register</p>
          <div className="border border-grey-200 p-5 space-y-4">

            <div>
              <label className={labelCls}>Known Register</label>
              <select
                className={inputCls}
                value={catalogue.find(r => r.address === writeAddrNum)?.address ?? ''}
                onChange={e => handleWriteAddressChange(e.target.value)}
                disabled={catalogueQuery.isPending}
              >
                <option value="">— select a register —</option>
                {catalogue.map(r => (
                  <option key={r.address} value={r.address}>
                    {r.name} · {r.address} / 0x{r.address.toString(16).toUpperCase().padStart(4, '0')} · {r.length}B
                  </option>
                ))}
              </select>
            </div>

            <div>
              <label className={labelCls}>Address (decimal)</label>
              <input
                type="number"
                min={0}
                max={65535}
                value={writeAddress}
                onChange={e => handleWriteAddressChange(e.target.value)}
                placeholder="e.g. 272"
                className={inputCls}
              />
              <p className="text-xs text-grey-500 mt-1 font-mono">{writeAddrHex}</p>
            </div>

            <div>
              <label className={labelCls}>Value (hex bytes)</label>
              <input
                type="text"
                value={writeValue}
                onChange={e => handleWriteValueChange(e.target.value)}
                placeholder="e.g. 0A FF or 0x0AFF"
                className={`${inputCls} font-mono`}
              />
              <p className="text-xs text-grey-500 mt-1 font-mono">
                {writeBytes === null
                  ? 'Enter an even number of hex digits.'
                  : `${writeBytes.length} byte(s): [${writeBytes.join(', ')}]`}
              </p>
            </div>

            <button onClick={handleWrite} disabled={!canWrite} className={btnCls}>
              {writing ? 'Writing…' : 'Write'}
            </button>

            {writeError && (
              <p className="text-xs text-status-bad font-mono">{writeError}</p>
            )}

            {writeOk && (
              <p className="text-xs text-status-good font-mono">
                Wrote {writeBytes?.length ?? 0} byte(s) to {writeAddrHex}.
              </p>
            )}
          </div>
        </section>

        </div>

        {/* Register catalogue */}
        <section>
          <p className="eyebrow mb-5">Register Catalogue</p>
          {catalogueQuery.isError && (
            <p className="text-xs text-status-bad font-mono">Failed to load register catalogue.</p>
          )}
          {catalogueQuery.isPending && (
            <p className="text-xs text-grey-600">Loading…</p>
          )}
          {catalogueQuery.isSuccess && (
            <div className="border border-grey-200 overflow-x-auto">
              <table className="w-full text-xs border-collapse">
                <thead>
                  <tr className="border-b border-grey-200 bg-grey-50">
                    {['Address', 'Hex', 'Length', 'Name', 'Description'].map(h => (
                      <th key={h} className="text-left px-4 py-2 font-display uppercase tracking-wide text-grey-600 font-medium whitespace-nowrap">
                        {h}
                      </th>
                    ))}
                  </tr>
                </thead>
                <tbody>
                  {catalogue.map(r => {
                    const isSelected = addrNum === r.address
                    return (
                      <tr
                        key={r.address}
                        onClick={() => { setAddress(String(r.address)); setLength(String(r.length)); setReadResult(null); setReadError(null) }}
                        className={`border-b border-grey-100 last:border-0 cursor-pointer transition-colors ${isSelected ? 'bg-grey-100' : 'hover:bg-grey-50'}`}
                      >
                        <td className="px-4 py-2 font-mono">{r.address}</td>
                        <td className="px-4 py-2 font-mono">0x{r.address.toString(16).toUpperCase().padStart(4, '0')}</td>
                        <td className="px-4 py-2 font-mono">{r.length}</td>
                        <td className={`px-4 py-2 font-mono ${isSelected ? 'text-syn-red' : ''}`}>{r.name}</td>
                        <td className="px-4 py-2 text-grey-600">{r.description}</td>
                      </tr>
                    )
                  })}
                </tbody>
              </table>
            </div>
          )}
        </section>

      </div>
    </div>
  )
}
