// CSV as RFC 4180 and Python's csv module write it: fields separated by commas, a field quoted only
// when it holds a comma, a quote or a line break, and each line ended with \r\n. So a file the
// TypeScript client writes is byte for byte the file the Python client writes. The index does not
// export this file.

/** Returns one CSV field. `null` and `undefined` are an empty field. */
export function csvField(value: unknown): string {
  const text = value === null || value === undefined ? '' : String(value);
  return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
}

/** Returns one CSV line, with its \r\n. */
export function csvLine(values: readonly unknown[]): string {
  return values.map(csvField).join(',') + '\r\n';
}
