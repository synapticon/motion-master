// CSV as RFC 4180 and Python's csv module write it: fields separated by commas, a field quoted only
// when it holds a comma, a quote or a line break, and each line ended with \r\n. The Python client
// writes the same layout. A number can still differ: JSON 2.0 is 2 in TypeScript and 2.0 in Python.
// The index does not export this file.

/** Returns one CSV field. `null` and `undefined` are an empty field. */
export function csvField(value: unknown): string {
  const text = value === null || value === undefined ? '' : String(value);
  return /[",\r\n]/.test(text) ? `"${text.replaceAll('"', '""')}"` : text;
}

/** Returns one CSV line, with its \r\n. */
export function csvLine(values: readonly unknown[]): string {
  return values.map(csvField).join(',') + '\r\n';
}
