import { useState } from 'react'
import { useQuery } from '@tanstack/react-query'
import AutoTuningExplainer from '../components/AutoTuningExplainer'
import FilePickerButton from '../components/FilePickerButton'
import PageHeader from '../components/PageHeader'
import { useConnection } from '../contexts/ConnectionContext'
import { btnOutline, btnPrimary } from '../utils/styles'

const inputCls = 'h-[38px] border border-grey-300 px-3 text-sm w-full bg-white'
const labelCls = 'block text-xs text-grey-500 mb-1'

// How often to re-read the status. It is a startup snapshot on the server and changes only when
// Motion Master restarts, so this is slow on purpose — it exists so a page left open after a
// restart stops claiming the old state.
const STATUS_POLL_MS = 15000

interface Example {
  /** Stable key for the picker. */
  id: string
  /** What the picker shows. */
  label: string
  /** One line on what the function does, and what the numbers in the example are. */
  description: string
  /** The request body, exactly as it is sent. */
  body: unknown
}

// Every function the endpoint forwards, with a request that runs. The values come from the
// auto-tuning program's own documentation, so they are the ones that repository publishes rather
// than numbers invented here — which matters, because a plausible-looking plant model is not a
// plant model. `exit` is absent: the endpoint refuses it, since it would stop the process for every
// other client.
const EXAMPLES: Example[] = [
  {
    id: 'auto_tune_velocity_controller',
    label: 'auto_tune_velocity_controller',
    description:
      'Velocity-loop gains from a plant model, a damping ratio and a demanded bandwidth in Hz.',
    body: {
      run: 'auto_tune_velocity_controller',
      data: {
        zeta: 1.1,
        demanded_bw: 5.0,
        plant_model: { numerator: [1.0], denominator: [0.00015, 0.000206] },
      },
    },
  },
  {
    id: 'auto_tune_position_controller',
    label: 'auto_tune_position_controller — standard rule',
    description:
      'Position-loop gains from a plant model and a demanded settling time in seconds. The controller type names the structure, here a PI velocity loop under a P position loop.',
    body: {
      run: 'auto_tune_position_controller',
      data: {
        zeta: 1.1,
        demanded_st: 0.2,
        controller_type: 'PI-P',
        plant_model: { numerator: [1.0], denominator: [0.00015, 0.000206] },
        highFrictionGain: 1,
      },
    },
  },
  {
    id: 'auto_tune_position_controller_high_friction',
    label: 'auto_tune_position_controller — high friction rule',
    description:
      'The same function against a machine with significant friction: a higher highFrictionGain and a third-order plant model, as the auto-tuning repository documents the rule.',
    body: {
      run: 'auto_tune_position_controller',
      data: {
        zeta: 1.0,
        demanded_st: 0.7,
        controller_type: 'PI-P',
        plant_model: {
          numerator: [0.0, 2.99066e-5, 0.00246124, 1.0],
          denominator: [2.94795e-10, 9.4479e-8, 0.000434015, 0.00925735],
        },
        highFrictionGain: 8,
      },
    },
  },
  {
    id: 'full_auto_tune_velocity_controller',
    label: 'full_auto_tune_velocity_controller',
    description:
      "Velocity-loop gains from the plant model plus the drive's own configuration — filters, notch, current-loop gains, DC link voltage, rated torque. Returns stability margins alongside the gains.",
    body: {
      run: 'full_auto_tune_velocity_controller',
      data: {
        plant_model: { numerator: [1.0], denominator: [0.00015, 0.000206] },
        drive_config: {
          vel_lp_type: 1,
          vel_lp_fc: 100,
          pos_lp_type: 1,
          pos_lp_fc: 50,
          notch_en: 0,
          notch_fw: 100,
          notch_fc: 100,
          notch_dp: 15,
          vel_ff_gain: 0,
          vel_ff_fc: 20,
          L: 153500,
          R: 300000,
          vdc: 29599,
          current_ratio: 32768.0,
          tc_kp: 2500.0,
          tc_ki: 40000.0,
          switch_F: 0,
          tc_st: 1400,
          tc_damping: 1400,
          software_version: 'v5.1.5',
          rated_torque: 289,
          max_vel_noise: 0.0,
        },
      },
    },
  },
  {
    id: 'full_auto_tune_position_controller',
    label: 'full_auto_tune_position_controller — standard rule',
    description:
      'Position-loop gains from the plant model and the drive configuration. highFriction selects the tuning rule: 0 is standard.',
    body: {
      run: 'full_auto_tune_position_controller',
      data: {
        plant_model: { numerator: [1.0], denominator: [0.00015, 0.000206] },
        controller_type: 'PI-P',
        drive_config: {
          vel_lp_type: 1,
          vel_lp_fc: 100,
          pos_lp_type: 1,
          pos_lp_fc: 50,
          notch_en: 0,
          notch_fw: 100,
          notch_fc: 100,
          notch_dp: 15,
          vel_ff_gain: 0,
          vel_ff_fc: 20,
          L: 153500,
          R: 300000,
          vdc: 29614,
          current_ratio: 32768.0,
          tc_kp: 2500.0,
          tc_ki: 40000.0,
          switch_F: 0,
          tc_st: 1400,
          tc_damping: 1400,
          software_version: 'v5.1.5',
          rated_torque: 289,
          max_vel_noise: 0.0,
        },
        highFriction: 0,
      },
    },
  },
  {
    id: 'full_auto_tune_position_controller_high_friction',
    label: 'full_auto_tune_position_controller — high friction rule',
    description:
      'The same function with highFriction set to 1, against the third-order plant model and the filter settings the auto-tuning repository documents for that rule.',
    body: {
      run: 'full_auto_tune_position_controller',
      data: {
        plant_model: {
          numerator: [0.0, 2.99066e-5, 0.00246124, 1.0],
          denominator: [2.94795e-10, 9.4479e-8, 0.000434015, 0.00925735],
        },
        controller_type: 'PI-P',
        drive_config: {
          vel_lp_type: 1,
          vel_lp_fc: 500,
          pos_lp_type: 1,
          pos_lp_fc: 1000,
          notch_en: 0,
          notch_fw: 100,
          notch_fc: 100,
          notch_dp: 15,
          vel_ff_gain: 0,
          vel_ff_fc: 20,
          L: 153500,
          R: 300000,
          vdc: 29614,
          current_ratio: 32768.0,
          tc_kp: 2500.0,
          tc_ki: 40000.0,
          switch_F: 0,
          tc_st: 1200,
          tc_damping: 1000,
          software_version: 'v5.1.5',
          rated_torque: 289,
          max_vel_noise: 0.0,
        },
        highFriction: 1,
      },
    },
  },
  {
    id: 'auto_tune_notch_none',
    label: 'auto_tune_notch — no resonance',
    description:
      'Looks for a mechanical resonance in the plant model and returns notch filter settings. This model has none, so the answer is notch_en false with null settings.',
    body: {
      run: 'auto_tune_notch',
      data: { plant_model: { numerator: [100], denominator: [1, 1] } },
    },
  },
  {
    id: 'auto_tune_notch_one',
    label: 'auto_tune_notch — one resonance',
    description:
      'The same function against a model that has a resonance: the answer carries the notch centre frequency, width and depth.',
    body: {
      run: 'auto_tune_notch',
      data: {
        plant_model: {
          numerator: [1.659e-5, 3.581e-3, 1.0],
          denominator: [4.85e-9, 1.258e-6, 7.726e-4, 0.1371],
        },
      },
    },
  },
  {
    id: 'auto_tune_feedback_filters',
    label: 'auto_tune_feedback_filters',
    description:
      'Cut-off frequencies for the position and velocity feedback filters, from the measured feedback noise, the encoder resolutions and the gear ratio.',
    body: {
      run: 'auto_tune_feedback_filters',
      data: { sigma_v: 0.025, sigma_p: 3.44, resVel: 524288, resPos: 524288, gearRatio: 80.0 },
    },
  },
  {
    id: 'compute_position_controller_gains',
    label: 'compute_position_controller_gains',
    description:
      'Position gains from design parameters rather than from a plant model. The auto-tuning repository documents no example for this one, so the field names come from its API description and the values below are only a set that returns a result — not a recommendation for any machine.',
    body: {
      run: 'compute_position_controller_gains',
      data: { pk: 1.0, pt: 0.001, omega: 100, alpha: 3, zeta: 1.0, controller_type: 'PI-P' },
    },
  },
  {
    id: 'identify_plant_model',
    label: 'identify_plant_model',
    description:
      'Fits a plant model to measurements the System Identification procedure recorded on the drive: three columns of CSV — time, torque in Nm, velocity in rad/s at 1000 Hz — plus the frequency range the chirp swept. Load a file below; its text is sent as data.csv.',
    body: {
      run: 'identify_plant_model',
      data: { csv: '', f0: 1, f1: 200 },
    },
  },
  {
    id: 'generate_plant_bode_file',
    label: 'generate_plant_bode_file',
    description:
      'Computes the bode data of a plant model and returns it inline as bode, rather than writing the CSV file the name suggests.',
    body: {
      run: 'generate_plant_bode_file',
      data: { plant_model: { numerator: [1.0], denominator: [0.00015, 0.000206] } },
    },
  },
]

function pretty(value: unknown): string {
  return JSON.stringify(value, null, 2)
}

interface Reply {
  status: number
  /** The body as text, pretty-printed when it parsed as JSON. */
  text: string
  /** True when the reply reports a failure: a non-2xx status, or an `error` property. */
  failed: boolean
  /** Round trip measured in the browser, so it includes this server and the child process. */
  wallMs: number
  /** The `duration` the auto-tuning program reported, in seconds, when it reported one. */
  duration?: number
}

export default function ToolsAutoTuningPage() {
  const { api } = useConnection()
  const [selectedId, setSelectedId] = useState(EXAMPLES[0].id)
  const [text, setText] = useState(() => pretty(EXAMPLES[0].body))
  const [csv, setCsv] = useState<{ name: string; text: string } | null>(null)
  const [running, setRunning] = useState(false)
  const [reply, setReply] = useState<Reply | null>(null)
  const [requestError, setRequestError] = useState<string | null>(null)

  const status = useQuery({
    queryKey: ['autoTuning'],
    queryFn: () => api.getAutoTuning(),
    refetchInterval: STATUS_POLL_MS,
  }).data?.data

  const example = EXAMPLES.find(e => e.id === selectedId) ?? EXAMPLES[0]
  const wantsCsv = example.id === 'identify_plant_model'

  function select(id: string) {
    const next = EXAMPLES.find(e => e.id === id) ?? EXAMPLES[0]
    setSelectedId(id)
    setText(pretty(next.body))
    setReply(null)
    setRequestError(null)
  }

  async function run() {
    setRunning(true)
    setReply(null)
    setRequestError(null)
    const startedAt = performance.now()
    try {
      const body: unknown = JSON.parse(text)
      // A measurement file is held outside the editor and spliced in here. Putting 200 KB of CSV in
      // a textarea makes every keystroke redraw it, and nobody edits those numbers by hand.
      if (csv && typeof body === 'object' && body !== null) {
        const data = (body as { data?: Record<string, unknown> }).data
        if (data && 'csv' in data) {
          data.csv = csv.text
        }
      }
      // A direct fetch rather than the generated client, because this page's job is to show what the
      // API answered. The generated client throws the body away into an exception for any non-2xx,
      // and those statuses — a refused input, an unknown function, a process that has died — are
      // exactly what somebody on this page is looking at.
      const response = await fetch(`${api.baseUrl}/api/auto-tuning/run`, {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(body),
      })
      const raw = await response.text()
      const wallMs = performance.now() - startedAt
      let parsed: unknown = null
      try {
        parsed = JSON.parse(raw)
      } catch {
        parsed = null
      }
      const object = typeof parsed === 'object' && parsed !== null ? (parsed as Record<string, unknown>) : null
      const duration = typeof object?.duration === 'number' ? object.duration : undefined
      setReply({
        status: response.status,
        text: parsed === null ? raw : pretty(parsed),
        // The auto-tuning program reports a rejected input with 200 and an `error` property, so the
        // status alone does not say whether the call did anything.
        failed: !response.ok || object?.error !== undefined,
        wallMs,
        duration,
      })
    } catch (err) {
      setRequestError(
        err instanceof SyntaxError
          ? `The request body is not valid JSON: ${err.message}`
          : err instanceof Error
            ? err.message
            : 'The request failed.',
      )
    } finally {
      setRunning(false)
    }
  }

  async function loadCsv(file: File) {
    setCsv({ name: file.name, text: await file.text() })
  }

  return (
    <div>
      <PageHeader
        eyebrow="Tools"
        title="Auto-Tuning"
        description="Call the tuning functions directly. Every one is reachable through a single endpoint, with a prepared example for each."
      />
      <div className="p-4 sm:px-8 sm:py-7 space-y-6">
        <AutoTuningExplainer />

        <div className="border border-grey-200 p-5 space-y-3">
          <h3 className="text-sm font-display uppercase">Process</h3>
          {!status && <p className="text-xs text-grey-600">Reading the status…</p>}
          {status && status.started && (
            <p className="text-xs text-grey-600">
              Version <span className="font-mono">{status.version || 'unknown'}</span> on port{' '}
              <span className="font-mono">{status.port}</span>, from{' '}
              <span className="font-mono">{status.binaryPath}</span>.
            </p>
          )}
          {status && !status.started && (
            <>
              <p className="text-xs text-status-bad">
                Auto-tuning is not running: {status.error || 'reason unknown'}.
              </p>
              <p className="text-xs text-grey-600">
                {!status.enabled
                  ? 'Set autoTuning.enabled in the configuration file and restart the server.'
                  : !status.installed
                    ? 'Run install-auto-tuning.sh (or setup.ps1 on Windows) next to the Motion Master binary, then restart the server. The executable is downloaded rather than shipped: it is about 65 MB and changes a few times a year.'
                    : 'The executable is installed but did not start. The server log and auto-tuning.log, next to it, say why.'}
              </p>
            </>
          )}
        </div>

        <div className="grid grid-cols-1 2xl:grid-cols-2 gap-6">
          <div className="border border-grey-200 p-5 space-y-4">
            <h3 className="text-sm font-display uppercase">Request</h3>

            <div>
              <label className={labelCls} htmlFor="auto-tuning-operation">
                Operation
              </label>
              <div className="flex items-center gap-3">
                <select
                  id="auto-tuning-operation"
                  value={selectedId}
                  onChange={e => select(e.target.value)}
                  className={inputCls}
                >
                  {EXAMPLES.map(e => (
                    <option key={e.id} value={e.id}>
                      {e.label}
                    </option>
                  ))}
                </select>
                <button
                  type="button"
                  onClick={() => select(selectedId)}
                  className={`${btnOutline} h-[38px] inline-flex items-center whitespace-nowrap`}
                  title="Discard edits and load the example again"
                >
                  Reset
                </button>
              </div>
            </div>

            <p className="text-xs text-grey-600">{example.description}</p>

            {wantsCsv && (
              <div className="flex items-center gap-3">
                <FilePickerButton
                  onFile={loadCsv}
                  accept=".csv,text/csv"
                  className="h-[38px]"
                  title="The file's text is sent as data.csv when you run"
                >
                  {csv ? 'Load another CSV' : 'Load measurement CSV'}
                </FilePickerButton>
                {csv && (
                  <span className="text-xs text-grey-500 font-mono">
                    {csv.name} · {csv.text.length} bytes
                  </span>
                )}
              </div>
            )}

            <div>
              <label className={labelCls} htmlFor="auto-tuning-body">
                Body
              </label>
              <textarea
                id="auto-tuning-body"
                value={text}
                onChange={e => setText(e.target.value)}
                spellCheck={false}
                rows={18}
                className="w-full border border-grey-300 p-3 font-mono text-xs bg-white"
              />
            </div>

            <div className="flex items-center justify-between gap-3">
              <button
                type="button"
                onClick={run}
                disabled={running || !status?.started}
                className={`${btnPrimary} h-[38px] inline-flex items-center`}
                title={status?.started ? undefined : 'No auto-tuning process is running'}
              >
                {running ? 'Running…' : 'Run'}
              </button>
              {reply && (
                <span className="text-xs text-grey-500 font-mono">
                  {reply.status} · {reply.wallMs.toFixed(0)} ms
                  {reply.duration !== undefined && ` · compute ${(reply.duration * 1000).toFixed(1)} ms`}
                </span>
              )}
            </div>

            {requestError && <p className="text-xs text-status-bad font-mono">{requestError}</p>}
          </div>

          <div className="border border-grey-200 p-5 space-y-4">
            <h3 className="text-sm font-display uppercase">Response</h3>
            {!reply && (
              <p className="text-xs text-grey-500">
                Nothing yet. Choose an operation and run it — these calls compute on the numbers in
                the body and touch no device.
              </p>
            )}
            {reply && (
              <>
                <p className={`text-xs ${reply.failed ? 'text-status-bad' : 'text-grey-600'}`}>
                  {reply.failed
                    ? 'The call did not produce a result. A refusal arrives with status 200 and an error property, so read the body rather than the status.'
                    : 'The call returned a result.'}
                </p>
                <pre className="border border-grey-200 bg-grey-50 p-3 font-mono text-xs overflow-auto max-h-[32rem] whitespace-pre">
                  {reply.text}
                </pre>
              </>
            )}
          </div>
        </div>
      </div>
    </div>
  )
}
