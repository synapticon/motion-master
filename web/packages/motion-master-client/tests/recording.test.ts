import { mkdtempSync, readFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
import { describe, expect, it } from 'vitest';
import { Cia402, MonitoringRecording } from '../src/index';

// The recording of the MonitoringRecording step of the Python client example in lint.yml, plus a row
// with a value that needs CSV quoting.
function recording() {
  return new MonitoringRecording(
    'ci',
    [
      { devicePosition: 1, index: 0x6041, subindex: 0, source: 'pdo' },
      { devicePosition: 2, index: 0x9999, subindex: 3, source: 'sdo' },
    ],
    [
      [1_000_000, 0x0237, 7],
      [1_001_000, null, 8],
      [1_002_500, 0x0637, 'a,b' as unknown as number],
    ],
  );
}

describe('MonitoringRecording', () => {
  it('names each column after the device and the generated object name', () => {
    expect(recording().names).toEqual(['1:STATUSWORD', '2:0x9999:03']);
    expect(recording().sources).toEqual(['pdo', 'sdo']);
  });

  it('reads one column, a time window and the first match', () => {
    const m = recording();
    expect(m.column(1, Cia402.STATUSWORD)).toEqual([
      [1_000_000, 0x0237],
      [1_001_000, null],
      [1_002_500, 0x0637],
    ]);
    expect(m.values(1, Cia402.STATUSWORD, { start: 1_001_000 })).toEqual([null, 0x0637]);
    expect(m.values(1, Cia402.STATUSWORD, { end: 1_001_000 })).toEqual([0x0237]);
    expect(m.first(1, Cia402.STATUSWORD, (v) => v === 0x0637)).toBe(1_002_500);
    expect(m.first(1, Cia402.STATUSWORD, (v) => v === 0x0237, { after: 1_000_001 })).toBeNull();
    expect(() => m.column(3, Cia402.STATUSWORD)).toThrow(
      'device 3 0x6041:00 is not in this monitoring',
    );
  });

  it('writes the same CSV bytes as the Python client', async () => {
    const path = join(mkdtempSync(join(tmpdir(), 'mm-')), 'sub', 'm.csv');
    await recording().writeCsv(path);
    // The bytes of MonitoringRecording.write_csv() of the Python client for the same rows.
    expect(readFileSync(path, 'utf8')).toBe(
      'time_s,timestamp_us,1:STATUSWORD,2:0x9999:03\r\n' +
        '0.000000,1000000,567,7\r\n' +
        '0.001000,1001000,,8\r\n' +
        '0.002500,1002500,1591,"a,b"\r\n',
    );
  });

  it('prints the same table as the Python client', () => {
    let out = '';
    recording().printTable({ everyMs: 1, file: { write: (text) => (out += text) } });
    // The output of MonitoringRecording.print_table() of the Python client for the same rows.
    expect(out).toBe(
      'time_s  1:STATUSWORD    1:CIA402_STATE  2:0x9999:03\n' +
        ' 0.000        0x0237  OperationEnabled            7\n' +
        ' 0.001             -                 -            8\n' +
        ' 0.003        0x0637  OperationEnabled          a,b\n',
    );
  });
});
