// The recording of a monitoring, as `MotionMasterClient.monitoring` returns it: the monitoring on the
// server, and every row it sent while the span ran.

import { Cia402 } from './cia402';
import { csvLine } from './csv';
import type { Monitoring } from './generated/data-contracts';
import { importNode, type NodeModules } from './node-modules';
import type { ObjectAddress } from './objects';
import { objectName } from './parameter-address';
import type { MonitoringRow } from './web-socket-connection';

/**
 * The recording of a monitoring: the monitoring on the server, and every row of the span.
 *
 * `MotionMasterClient.monitoring` creates the monitoring, records it while its span runs, and
 * deletes it. The rows stay readable afterwards.
 */
export class MonitoringRecording {
  /**
   * @param topic The topic of the monitoring.
   * @param parameters The parameters as the server answered them, each with its source.
   * @param rows Every row, `[timestampUs, v0, v1, ...]` in the order of `parameters`.
   */
  constructor(
    readonly topic: string,
    readonly parameters: Monitoring['parameters'],
    readonly rows: MonitoringRow[] = [],
  ) {}

  /** The source of each parameter, in order: `'pdo'` or `'sdo'`. */
  get sources(): ('pdo' | 'sdo')[] {
    return this.parameters.map((p) => p.source);
  }

  /**
   * The name of each parameter's column, in order: its device position and its object.
   *
   * The object is its generated name, `1:STATUSWORD`, or `1:0x2110:03` for an object with no
   * generated name. `writeCsv()` and `printTable()` name their columns with these.
   */
  get names(): string[] {
    return this.parameters.map((p) => `${p.devicePosition}:${objectName(p.index, p.subindex)}`);
  }

  /** Returns `[timestampUs, value]` of one object for every row, in the order the cycles ran. */
  column<T>(devicePosition: number, address: ObjectAddress<T>): [number, T | null][] {
    const i = this.parameters.findIndex(
      (p) =>
        p.devicePosition === devicePosition &&
        p.index === address.index &&
        p.subindex === address.subindex,
    );
    if (i < 0) {
      const index = address.index.toString(16).padStart(4, '0');
      const subindex = address.subindex.toString(16).padStart(2, '0');
      throw new Error(`device ${devicePosition} 0x${index}:${subindex} is not in this monitoring`);
    }
    return this.rows.map((row) => [row[0] as number, row[i + 1] as T | null]);
  }

  /**
   * Returns the values of one object from the cycles with `start <= timestampUs < end`.
   * Without `start` or `end`, the window is open on that side, so `values(position, address)`
   * gives every value.
   */
  values<T>(
    devicePosition: number,
    address: ObjectAddress<T>,
    options: { start?: number | null; end?: number | null } = {},
  ): (T | null)[] {
    const { start, end } = options;
    return this.column(devicePosition, address)
      .filter(([t]) => (start == null || t >= start) && (end == null || t < end))
      .map(([, v]) => v);
  }

  /**
   * Returns the `timestampUs` of the first cycle where `predicate(value)` is true, or `null`.
   * With `after`, only the cycles with `timestampUs >= after` count.
   */
  first<T>(
    devicePosition: number,
    address: ObjectAddress<T>,
    predicate: (value: T | null) => unknown,
    options: { after?: number | null } = {},
  ): number | null {
    const { after } = options;
    for (const [t, v] of this.column(devicePosition, address)) {
      if ((after == null || t >= after) && predicate(v)) {
        return t;
      }
    }
    return null;
  }

  /**
   * Writes every row to a CSV file at `path`, and creates its folder when it is missing. Node.js
   * only.
   *
   * The columns are time_s, the seconds from the first row; timestamp_us, the time on the server
   * when the cycle ran, in epoch microseconds; then one column per parameter, named as in `names`.
   * A value is empty in a cycle where its device did not exchange process data.
   */
  async writeCsv(path: string): Promise<void> {
    const { mkdirSync, writeFileSync } = await importNode<NodeModules['fs']>('node:fs');
    const { dirname } = await importNode<NodeModules['path']>('node:path');
    mkdirSync(dirname(path), { recursive: true });
    const start = (this.rows[0]?.[0] as number | undefined) ?? 0;
    const lines = [csvLine(['time_s', 'timestamp_us', ...this.names])];
    for (const [timestampUs, ...values] of this.rows) {
      const t = timestampUs as number;
      lines.push(csvLine([((t - start) / 1e6).toFixed(6), t, ...values]));
    }
    writeFileSync(path, lines.join(''));
  }

  /**
   * Prints the rows as a table: the first row, then one row every `everyMs`, 100 when not given.
   *
   * The columns are time_s, the seconds from the first row, then one column per parameter, named as
   * in `names`. A Statusword (0x6041) is printed in hex, and its column is followed by a column with
   * the CiA402 state it reports. A value is - in a cycle where its device did not exchange process
   * data. `file` is where the table goes: anything with `write(text)`, such as `process.stdout`.
   * Without it, each line goes to `console.log`.
   */
  printTable(options: { everyMs?: number; file?: { write(text: string): unknown } } = {}): void {
    const { everyMs = 100, file } = options;
    const statusword = Cia402.STATUSWORD;
    const isStatusword = this.parameters.map(
      (p) => p.index === statusword.index && p.subindex === statusword.subindex,
    );
    const header = ['time_s'];
    this.names.forEach((name, i) => {
      header.push(name);
      if (isStatusword[i]) {
        header.push(`${this.parameters[i].devicePosition}:CIA402_STATE`);
      }
    });
    const lines: string[][] = [];
    const start = (this.rows[0]?.[0] as number | undefined) ?? 0;
    let nextUs = start;
    for (const [timestampUs, ...values] of this.rows) {
      const t = timestampUs as number;
      if (t < nextUs) {
        continue;
      }
      nextUs = t + everyMs * 1000;
      const line = [((t - start) / 1e6).toFixed(3)];
      values.forEach((value, i) => {
        if (value === null) {
          line.push(...(isStatusword[i] ? ['-', '-'] : ['-']));
        } else if (isStatusword[i]) {
          const hex = value.toString(16).toUpperCase().padStart(4, '0');
          line.push(`0x${hex}`, Cia402.state(value));
        } else {
          line.push(String(value));
        }
      });
      lines.push(line);
    }
    const table = [header, ...lines];
    const widths = header.map((_, c) => Math.max(...table.map((row) => (row[c] ?? '').length)));
    for (const row of table) {
      const text = row.map((cell, c) => cell.padStart(widths[c])).join('  ');
      if (file) {
        file.write(text + '\n');
      } else {
        console.log(text);
      }
    }
  }
}
