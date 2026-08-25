/**
 * The safe sensor's invalidation vocabulary: SafeSensorInvalidReason and the two
 * ways of showing it.
 *
 * Shared because two panels need it for the same fact from opposite directions.
 * The safe-sensor panel reports the CHANNEL's health; the SS1 stop trace has to
 * explain a span where the velocity validity flag was false, and the flag is all
 * the safety PDU carries - ETG.6100.2 ch. 5.4 has a validity bit and no cause.
 * The cause lives in the non-safe diagnosis objects (0x2601/0x2602/0x2603:
 * reason, causes, invalidation count), which is where ETG.6100 expects it, so
 * both panels are decoding the same enum out of the same place.
 */

// SafeSensorInvalidReason. The three "not a fault" cross-check states are
// styled as information rather than as problems, because a single-encoder axis
// reports one of them for the whole of its life and it is not broken.
export const REASON: Record<number, { label: string; tone: 'ok' | 'info' | 'warn' | 'bad'; hint: string }> = {
  0:  { label: 'Ok', tone: 'ok', hint: 'The value is valid.' },
  1:  { label: 'No sample', tone: 'info', hint: 'The pipeline has taken no coherent sample yet. With no safety master connected it is dormant, which is the resting state rather than a fault.' },
  2:  { label: 'Stale', tone: 'bad', hint: 'Acquisition stopped presenting new samples past the sample hold timeout.' },
  3:  { label: 'Sensor fault', tone: 'bad', hint: 'The acquisition flagged the reading itself as bad.' },
  4:  { label: 'Window filling', tone: 'warn', hint: 'An averaging window has not filled. The average would read low, which understates speed or torque - the dangerous direction - so the value is withheld until it has.' },
  5:  { label: 'Position discrepancy', tone: 'bad', hint: 'The channels’ positions differ by more than the allowed discrepancy, for longer than the debounce.' },
  6:  { label: 'Implausible currents', tone: 'bad', hint: 'The three phase currents do not sum to zero within tolerance.' },
  7:  { label: 'Unconfigured', tone: 'bad', hint: 'The configuration was rejected; no sample can change that until it is fixed.' },
  8:  { label: 'Parameters unvalidated', tone: 'warn', hint: 'The safety parameters have not been agreed with the master, so the scaling is unagreed.' },
  9:  { label: 'Bad primary encoder', tone: 'bad', hint: 'Primary resolution, or the wrap derived from it, is out of range.' },
  10: { label: 'Bad verification encoder', tone: 'bad', hint: 'Verification resolution, or the wrap derived from it, is out of range.' },
  11: { label: 'Bad gear ratio', tone: 'bad', hint: 'A gear term is larger than the pipeline can carry.' },
  12: { label: 'No cycle period', tone: 'bad', hint: 'The safety cycle period is zero, so the filter windows have no length.' },
  13: { label: 'Bad torque scaling', tone: 'bad', hint: 'Torque constant or current ratio is not positive.' },
  14: { label: 'Scaling overflow', tone: 'bad', hint: 'The reduced position scaling does not fit the arithmetic budget.' },
  15: { label: 'Invalidated', tone: 'bad', hint: 'Something invalidated the pipeline; the absolute reference is latched lost.' },
  16: { label: 'Inbox torn', tone: 'bad', hint: 'The sampler is publishing faster than the pipeline can read a coherent snapshot.' },
  17: { label: 'Primary count out of range', tone: 'bad', hint: 'The raw count lies outside the configured resolution - a commissioning error, not a sick sensor.' },
  18: { label: 'Verification count out of range', tone: 'bad', hint: 'As above, for the verification channel.' },
  19: { label: 'Velocity discrepancy', tone: 'bad', hint: 'The channels’ velocities differ by more than the allowed discrepancy.' },
  20: { label: 'Verification channel down', tone: 'bad', hint: 'The verification channel is not delivering, so no comparison can pass. Widening a tolerance will not help.' },
  21: { label: 'Current out of range', tone: 'bad', hint: 'A phase magnitude exceeds what the acquisition can represent.' },
  22: { label: 'No verification channel', tone: 'info', hint: 'One encoder is fitted, so there is nothing to cross-check. Not a fault - this is the correct report for a single-channel axis.' },
  23: { label: 'Cross-check disabled', tone: 'warn', hint: 'Two channels are fitted and both tolerances are zero, so nothing is being compared. Almost always a commissioning omission.' },
  24: { label: 'Reference not captured', tone: 'info', hint: 'The alignment offset between the channels has never been taken.' },
  25: { label: 'Reference lost', tone: 'bad', hint: 'The reference was lost and no later sample can recover it.' },
  26: { label: 'Velocity saturated', tone: 'warn', hint: 'The velocity clipped its numeric rail. The value stays valid.' },
  27: { label: 'Torque saturated', tone: 'warn', hint: 'The torque clipped its numeric rail. The value stays valid.' },
  28: { label: 'Position rollover', tone: 'info', hint: 'The position accumulator wrapped its multiturn range. Expected on a continuously rotating axis.' },
}

export const TONE_CLS = {
  ok:   'bg-status-good text-white',
  info: 'bg-grey-200 text-grey-700',
  warn: 'bg-status-warn text-grey-900',
  bad:  'bg-status-bad text-white',
}

export function ReasonChip({ code }: { code: number | undefined }) {
  if (code === undefined) return null
  const r = REASON[code] ?? { label: `Reason ${code}`, tone: 'warn' as const, hint: 'Unknown reason code.' }
  return (
    <span title={r.hint} className={`inline-flex items-center h-[18px] px-1.5 text-[10px] tracking-wide cursor-help ${TONE_CLS[r.tone]}`}>
      {r.label}
    </span>
  )
}

/** Every reason ever seen since this connection came up, as chips. */
export function CauseChips({ mask }: { mask: number | undefined }) {
  if (!mask) return null
  const seen = Object.keys(REASON)
    .map(Number)
    .filter(c => c !== 0 && (mask & (1 << c)) !== 0)
  if (seen.length === 0) return null
  return (
    <div className="mt-2 flex flex-wrap gap-1">
      <span className="text-[10px] uppercase tracking-wider text-grey-500 mr-1">seen</span>
      {seen.map(c => (
        <span key={c} title={REASON[c].hint}
              className="inline-flex items-center h-[16px] px-1 text-[9px] border border-grey-300 text-grey-600 cursor-help">
          {REASON[c].label}
        </span>
      ))}
    </div>
  )
}
