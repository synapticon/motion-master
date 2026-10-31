import { useEffect, useRef, useState } from 'react'
import { useParams } from 'react-router'
import { useMutation, useQueries, useQuery, useQueryClient } from '@tanstack/react-query'
import {
  apiErrorMessage,
  type FirmwareCompatibility,
  type FirmwarePackageEntry,
  type ProcedureRequest,
  type ProcedureSnapshot,
  type ProgressStep,
} from '@synapticon/motion-master-client'
import Callout from '../components/Callout'
import DevicePageHeader from '../components/DevicePageHeader'
import HardwareDescriptionView from '../components/HardwareDescriptionView'
import IntegroVariantView from '../components/IntegroVariantView'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline, btnPrimary } from '../utils/styles'
import { userCacheUrl } from '../utils/userCache'

const PROCEDURE = 'firmware-installation'
const CACHE_DIRECTORY = 'firmwares/'

const inputCls = 'border border-grey-300 px-3 h-[38px] text-sm bg-white'
const labelCls = 'block text-xs text-grey-600 mb-1 uppercase tracking-wide'

const STEP_DOT: Record<ProgressStep['status'], string> = {
  idle: 'bg-grey-300',
  running: 'bg-ocean',
  succeeded: 'bg-green-600',
  failed: 'bg-syn-red',
}

// How the page names each kind of entry an install always writes.
const ALWAYS_WRITTEN: Record<Exclude<FirmwarePackageEntry['kind'], 'extra-file'>, string> = {
  'app-firmware': 'application firmware',
  'com-firmware': 'communication firmware',
  sii: 'SII',
}

// What each step of the installation does, for a reader who has not read the procedure.
const STEP_TITLE: Record<string, string> = {
  package: 'Read the package',
  cache: 'Keep the package on the server',
  boot: 'Enter the bootloader (BOOT)',
  'extra-files': 'Write the extra files',
  sii: 'Write the SII',
  'app-firmware': 'Write the application firmware',
  'com-firmware': 'Write the communication firmware',
  'activate-firmware': 'Activate the firmware',
  'final-state': 'Leave the device in the final state',
}

function isNotFound(error: unknown): boolean {
  return typeof error === 'object' && error !== null && (error as { status?: unknown }).status === 404
}

function baseName(path: string): string {
  return path.slice(path.lastIndexOf('/') + 1)
}

function stepValueText(value: unknown): string {
  if (value === undefined || value === null) {
    return ''
  }
  return typeof value === 'string' ? value : JSON.stringify(value)
}

export default function DeviceFirmwarePage() {
  const { deviceId } = useParams()
  const slavePosition = Number(deviceId)
  const { api } = useConnection()
  const queryClient = useQueryClient()
  const fileInput = useRef<HTMLInputElement>(null)

  const versionQuery = useQuery({
    queryKey: ['firmware-version', slavePosition],
    queryFn: () => api.readParameter(slavePosition, 0x100a, 0).then(r => String(r.data.value)),
    retry: false,
  })
  const hardwareQuery = useQuery({
    queryKey: ['hardware-description', slavePosition],
    queryFn: () => api.getDeviceHardwareDescription(slavePosition).then(r => r.data),
    retry: false,
  })
  const descriptorsQuery = useQuery({
    queryKey: ['firmware-descriptors', slavePosition],
    queryFn: () => api.getDeviceFirmwareDescriptors(slavePosition).then(r => r.data),
    retry: false,
  })
  // Only an Integro carries a .variant, so a 404 is an answer, not a failure.
  const variantQuery = useQuery({
    queryKey: ['integro-variant', slavePosition],
    queryFn: () => api.getDeviceIntegroVariant(slavePosition).then(r => r.data),
    retry: false,
  })
  const proceduresQuery = useQuery({
    queryKey: ['procedures', slavePosition],
    queryFn: () => api.listProcedures(slavePosition).then(r => r.data),
    retry: false,
  })
  const cacheQuery = useQuery({
    queryKey: ['user-cache'],
    queryFn: () => api.listUserCacheFiles().then(r => r.data),
    retry: false,
  })
  const snapshotQuery = useQuery({
    queryKey: ['procedure', slavePosition, PROCEDURE],
    queryFn: () => api.getProcedure(slavePosition, PROCEDURE).then(r => r.data),
    refetchInterval: query => (query.state.data?.status === 'running' ? 1000 : false),
    refetchIntervalInBackground: true,
    retry: false,
  })

  const packages = (cacheQuery.data?.files ?? [])
    .map(file => file.path)
    .filter(path => path.startsWith(CACHE_DIRECTORY) && path.toLowerCase().endsWith('.zip'))
    .map(baseName)
    .sort()

  // The server's verdict for each package. The page matches nothing itself.
  const verdicts = useQueries({
    queries: packages.map(filename => ({
      queryKey: ['firmware-compatibility', slavePosition, filename],
      queryFn: () =>
        api.checkDeviceFirmwareCompatibility(slavePosition, { filename }).then(r => r.data),
      staleTime: Infinity,
      retry: false,
    })),
  })

  const descriptor = proceduresQuery.data?.find(
    listing => listing.descriptor.name === PROCEDURE,
  )?.descriptor
  const finalStateParameter = descriptor?.parameters.find(p => p.name === 'finalState')
  const extraFilesParameter = descriptor?.parameters.find(p => p.name === 'extraFiles')

  const [finalState, setFinalState] = useState<number | undefined>()
  const [selected, setSelected] = useState<string | undefined>()
  // The choice belongs to one package, so choosing another package starts with none selected.
  const [extraFiles, setExtraFiles] = useState<{ filename?: string; names: string[] }>({
    names: [],
  })
  const finalStateValue = finalState ?? (finalStateParameter?.defaultValue as number | undefined)
  const extraFilesValue = extraFiles.filename === selected ? extraFiles.names : []

  const entriesQuery = useQuery({
    queryKey: ['firmware-package-entries', selected],
    queryFn: () =>
      api.listFirmwarePackageEntries({ filename: selected as string }).then(r => r.data.entries),
    enabled: selected !== undefined,
    staleTime: Infinity,
    retry: false,
  })
  const toggleExtraFile = (name: string, checked: boolean) => {
    setExtraFiles({
      filename: selected,
      names: checked
        ? [...extraFilesValue, name]
        : extraFilesValue.filter(other => other !== name),
    })
  }

  const upload = useMutation({
    // The generated client JSON-encodes request bodies, which turns a File into "{}", so the raw
    // bytes go up with fetch, as on the User Cache page.
    mutationFn: async (file: File) => {
      const res = await fetch(userCacheUrl(api.baseUrl, `${CACHE_DIRECTORY}${file.name}`), {
        method: 'PUT',
        headers: { 'Content-Type': 'application/octet-stream' },
        body: file,
      })
      if (!res.ok) {
        const body = await res.json().catch(() => null)
        throw new Error(body?.error ?? `HTTP ${res.status}`)
      }
    },
    onSuccess: (_data, file) => {
      setSelected(file.name)
      void queryClient.invalidateQueries({ queryKey: ['user-cache'] })
    },
  })
  const install = useMutation({
    mutationFn: (packageFilename: string) =>
      api.startProcedure(slavePosition, PROCEDURE, {
        packageFilename,
        ...(finalStateValue !== undefined
          ? { finalState: finalStateValue as ProcedureRequest['finalState'] }
          : {}),
        extraFiles: extraFilesValue,
      }),
    onSettled: () =>
      queryClient.invalidateQueries({ queryKey: ['procedure', slavePosition, PROCEDURE] }),
  })

  const snapshot = snapshotQuery.data
  const running = snapshot?.status === 'running' || install.isPending

  // The device runs other firmware after a run, so its version is read again.
  const finishedAt = snapshot?.finishedAt
  useEffect(() => {
    if (finishedAt) {
      void queryClient.invalidateQueries({ queryKey: ['firmware-version', slavePosition] })
    }
  }, [finishedAt, queryClient, slavePosition])

  const selectedIndex = selected ? packages.indexOf(selected) : -1
  const selectedVerdict = selectedIndex >= 0 ? verdicts[selectedIndex]?.data : undefined
  const deviceDescriptors = descriptorsQuery.data
  const descriptorText = (value: string | undefined) =>
    descriptorsQuery.isPending
      ? 'Reading…'
      : descriptorsQuery.isError
        ? apiErrorMessage(descriptorsQuery.error)
        : (value ?? '—')

  return (
    <div>
      <DevicePageHeader
        slavePosition={slavePosition}
        title="Firmware"
        description="The firmware this device runs, the packages that fit it, and the installation of one of them."
      />

      <div className="p-4 sm:px-8 sm:py-7 space-y-6 max-w-5xl">
        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200">
            <h3 className="font-display uppercase tracking-wide text-sm">This device</h3>
          </div>
          <dl className="grid grid-cols-1 sm:grid-cols-3 gap-x-6 gap-y-3 px-5 py-4 text-sm">
            <Field
              label="Firmware version (0x100A)"
              value={
                versionQuery.isPending
                  ? 'Reading…'
                  : versionQuery.isError
                    ? apiErrorMessage(versionQuery.error)
                    : (versionQuery.data ?? '—')
              }
            />
            <Field label="Device descriptor" value={descriptorText(deviceDescriptors?.device)} />
            <Field
              label="Assembly descriptor"
              value={descriptorText(
                deviceDescriptors ? (deviceDescriptors.assembly ?? 'no assembly') : undefined,
              )}
            />
          </dl>
          <div className="px-5 pb-4 space-y-4">
            {hardwareQuery.isError && (
              <Callout variant="error">{apiErrorMessage(hardwareQuery.error)}</Callout>
            )}
            {hardwareQuery.data && <HardwareDescriptionView description={hardwareQuery.data} />}
            {variantQuery.data && <IntegroVariantView variant={variantQuery.data} />}
            {variantQuery.isError && !isNotFound(variantQuery.error) && (
              <Callout variant="error">{apiErrorMessage(variantQuery.error)}</Callout>
            )}
          </div>
        </section>

        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
            <h3 className="font-display uppercase tracking-wide text-sm">Packages</h3>
            <div className="flex items-center gap-3">
              <input
                ref={fileInput}
                type="file"
                accept=".zip"
                className="hidden"
                onChange={event => {
                  const file = event.target.files?.[0]
                  if (file) {
                    upload.mutate(file)
                  }
                  event.target.value = ''
                }}
              />
              <button
                type="button"
                className={btnOutline}
                disabled={upload.isPending || running}
                onClick={() => fileInput.current?.click()}
              >
                {upload.isPending ? 'Adding…' : 'Add a package from disk'}
              </button>
            </div>
          </div>
          <p className="px-5 pt-4 text-xs text-grey-600">
            The packages the server keeps in {CACHE_DIRECTORY} of its user cache
            {cacheQuery.data ? ` (${cacheQuery.data.root})` : ''}, each checked against this
            device by the server. A package added from disk is kept there too.
          </p>
          {upload.isError && (
            <div className="px-5 pt-3">
              <Callout variant="error">{apiErrorMessage(upload.error)}</Callout>
            </div>
          )}
          {cacheQuery.isError && (
            <div className="px-5 pt-3">
              <Callout variant="error">{apiErrorMessage(cacheQuery.error)}</Callout>
            </div>
          )}
          {cacheQuery.isSuccess && packages.length === 0 && (
            <p className="px-5 py-4 text-sm">No package yet. Add one from disk.</p>
          )}
          {packages.length > 0 && (
            <table className="w-full text-sm mt-2">
              <tbody>
                {packages.map((filename, index) => (
                  <PackageRow
                    key={filename}
                    filename={filename}
                    verdict={verdicts[index]?.data}
                    error={verdicts[index]?.error}
                    pending={verdicts[index]?.isPending ?? true}
                    selected={selected === filename}
                    onSelect={() => setSelected(filename)}
                  />
                ))}
              </tbody>
            </table>
          )}
        </section>

        <section className="border border-grey-200 bg-white">
          <div className="px-5 py-3 border-b border-grey-200">
            <h3 className="font-display uppercase tracking-wide text-sm">Install</h3>
          </div>
          {descriptor && descriptor.caveats.length > 0 && (
            <ul className="px-5 pt-4 space-y-1 text-xs text-grey-700 list-disc list-inside">
              {descriptor.caveats.map(caveat => (
                <li key={caveat}>{caveat}</li>
              ))}
            </ul>
          )}
          <div className="flex flex-wrap items-end gap-4 px-5 py-4">
            <div>
              <label className={labelCls} htmlFor="firmware-package">
                Package
              </label>
              <select
                id="firmware-package"
                className={`${inputCls} max-w-[32rem]`}
                value={selected ?? ''}
                onChange={event => setSelected(event.target.value || undefined)}
              >
                <option value="">Choose a package</option>
                {packages.map(filename => (
                  <option key={filename} value={filename}>
                    {filename}
                  </option>
                ))}
              </select>
            </div>
            {finalStateParameter && (
              <div>
                <label className={labelCls} htmlFor="firmware-final-state">
                  Final state
                </label>
                <select
                  id="firmware-final-state"
                  className={inputCls}
                  value={finalStateValue}
                  onChange={event => setFinalState(Number(event.target.value))}
                >
                  {(finalStateParameter.options ?? []).map(option => (
                    <option key={String(option.value)} value={Number(option.value)}>
                      {option.title}
                    </option>
                  ))}
                </select>
              </div>
            )}
          </div>
          {extraFilesParameter && selected && (
            <details className="px-5 pb-4">
              <summary className="text-xs text-grey-600 cursor-pointer">Advanced</summary>
              <p className={`${labelCls} mt-3`}>Files to write</p>
              {entriesQuery.isPending && <p className="text-sm">Reading the package…</p>}
              {entriesQuery.isError && (
                <Callout variant="error">{apiErrorMessage(entriesQuery.error)}</Callout>
              )}
              {entriesQuery.data && (
                <ul className="space-y-1 text-sm">
                  {entriesQuery.data.map(entry => (
                    <li key={entry.name}>
                      {entry.kind === 'extra-file' ? (
                        <label className="flex items-center gap-2 cursor-pointer">
                          <input
                            type="checkbox"
                            checked={extraFilesValue.includes(entry.name)}
                            disabled={running}
                            onChange={event => toggleExtraFile(entry.name, event.target.checked)}
                          />
                          <span className="font-mono">{entry.name}</span>
                        </label>
                      ) : (
                        <span className="flex items-center gap-2">
                          <input type="checkbox" checked disabled className="invisible" />
                          <span className="font-mono">{entry.name}</span>
                          <span className="text-xs text-grey-600">
                            {ALWAYS_WRITTEN[entry.kind]}, always written
                          </span>
                        </span>
                      )}
                    </li>
                  ))}
                </ul>
              )}
              <p className="text-xs text-grey-600 mt-2">{extraFilesParameter.description}</p>
            </details>
          )}
          {selectedVerdict && !selectedVerdict.compatible && (
            <div className="px-5 pb-4">
              <Callout variant="warning">
                This package is built for other hardware. {selectedVerdict.explanation} Installing
                it anyway is allowed: the installation writes what it is given.
              </Callout>
            </div>
          )}
          {install.isError && (
            <div className="px-5 pb-4">
              <Callout variant="error">{apiErrorMessage(install.error)}</Callout>
            </div>
          )}
          <div className="flex flex-wrap items-center gap-3 px-5 py-4 border-t border-grey-100">
            <button
              type="button"
              className={btnPrimary}
              disabled={!selected || running}
              onClick={() => selected && install.mutate(selected)}
            >
              {running ? 'Installing…' : 'Install'}
            </button>
            <span className="text-xs text-grey-600">
              {snapshot?.startedAt && snapshot.finishedAt
                ? `Last install took ${((snapshot.finishedAt - snapshot.startedAt) / 1000).toFixed(0)} s, ${snapshot.status}`
                : 'Not installed from here yet'}
            </span>
          </div>
        </section>

        {snapshot && snapshot.status !== 'idle' && <InstallRun snapshot={snapshot} />}
      </div>
    </div>
  )
}

function PackageRow({
  filename,
  verdict,
  error,
  pending,
  selected,
  onSelect,
}: {
  filename: string
  verdict: FirmwareCompatibility | undefined
  error: unknown
  pending: boolean
  selected: boolean
  onSelect: () => void
}) {
  return (
    <tr
      className={`border-t border-grey-100 align-top cursor-pointer ${selected ? 'bg-ocean/5' : ''}`}
      onClick={onSelect}
    >
      <td className="pl-5 py-2 font-mono text-xs break-all">{filename}</td>
      <td className="px-3 py-2 text-xs whitespace-nowrap">
        {verdict?.packageName
          ? `${verdict.packageName.softwareName} ${verdict.packageName.softwareVersion}`
          : ''}
      </td>
      <td className="pr-5 py-2 text-xs">
        {pending ? (
          <span className="text-grey-500">checking…</span>
        ) : verdict ? (
          verdict.compatible ? (
            <span className="text-status-good">fits this device ({verdict.match})</span>
          ) : (
            <span className="text-status-warn">built for other hardware</span>
          )
        ) : (
          <span className="text-grey-600">{apiErrorMessage(error)}</span>
        )}
      </td>
    </tr>
  )
}

function InstallRun({ snapshot }: { snapshot: ProcedureSnapshot }) {
  const failed = snapshot.steps.find(step => step.status === 'failed')
  return (
    <section className="border border-grey-200 bg-white">
      <div className="px-5 py-3 border-b border-grey-200 flex items-center justify-between">
        <h3 className="font-display uppercase tracking-wide text-sm">Last install</h3>
        <span className="text-xs text-grey-600">{snapshot.status}</span>
      </div>
      <table className="w-full text-xs">
        <tbody>
          {snapshot.steps.map(step => (
            <tr key={step.id} className="border-b border-grey-100 last:border-0 align-top">
              <td className="pl-5 py-2 w-6">
                <span
                  className={`inline-block h-2 w-2 rounded-full ${STEP_DOT[step.status]} ${
                    step.status === 'running' ? 'animate-pulse' : ''
                  }`}
                />
              </td>
              <td className="px-3 py-2">
                <div>{STEP_TITLE[step.id] ?? step.id}</div>
                <div className="font-mono text-grey-500">{step.id}</div>
              </td>
              <td className="px-3 py-2">{step.status}</td>
              <td className="px-3 py-2 break-all">
                {step.error ? (
                  <span className="text-status-bad">{step.error}</span>
                ) : (
                  <span className="text-grey-700">{stepValueText(step.value)}</span>
                )}
              </td>
            </tr>
          ))}
        </tbody>
      </table>
      {failed?.error && (
        <div className="px-5 py-3 border-t border-grey-100">
          <Callout variant="error">{failed.error}</Callout>
        </div>
      )}
    </section>
  )
}

function Field({ label, value, hint }: { label: string; value: string; hint?: string }) {
  return (
    <div>
      <dt className={labelCls}>{label}</dt>
      <dd className="font-mono break-all">{value}</dd>
      {hint && <dd className="text-xs text-grey-500">{hint}</dd>}
    </div>
  )
}
