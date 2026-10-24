import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { parseCsv } from './Analyze-ConcurrentRun.mjs';

export function selectMeasurement(rows, window) {
  if (!window.complete) throw new Error('Measurement window is incomplete.');
  const start = BigInt(window.start_qpc_ticks), end = BigInt(window.end_qpc_ticks);
  const frequency = Number(window.qpc_frequency);
  if (end <= start || !Number.isFinite(frequency) || frequency <= 0) throw new Error('Invalid QPC measurement window.');
  const selected = [], seen = new Set();
  let invalid = 0, outside = 0, boundary = 0;
  const ordered = rows.flatMap(row => {
    if (!/^\d+$/.test(row.QPCTime || '')) { ++invalid; return []; }
    return [{row, qpc: BigInt(row.QPCTime)}];
  }).sort((a,b) => a.qpc < b.qpc ? -1 : a.qpc > b.qpc ? 1 : 0);
  for (const {row, qpc} of ordered) {
    if (qpc < start || qpc > end) { ++outside; continue; }
    const stream = `${row.ProcessID}:${row.SwapChainAddress}`;
    // The first interval may have begun during startup/calibration. Require a
    // previous present in the measured window for each independent stream.
    if (!seen.has(stream)) { seen.add(stream); ++boundary; continue; }
    selected.push({...row, TimeInSeconds: (Number(qpc - start) / frequency).toFixed(9)});
  }
  if (!selected.length) throw new Error('No complete presentation intervals inside the measurement window. Capture with PresentMon 1.5.2 -qpc_time.');
  return {rows: selected, metadata: {
    alignment: 'Windows QueryPerformanceCounter ticks shared by game and PresentMon',
    window_seconds: Number(end - start) / frequency, total_rows: rows.length,
    selected_rows: selected.length, outside_window_rows: outside,
    first_boundary_rows_dropped: boundary, invalid_qpc_rows: invalid,
    time_origin: 'game measurement start; startup and inference warmup excluded',
    presentmon_reference: 'https://github.com/GameTechDev/PresentMon/blob/v1.5.2/README.md#csv-columns'
  }};
}
const csv = value => '"' + String(value ?? '').replaceAll('"', '""') + '"';
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try {
    const [input, measurement, output] = process.argv.slice(2);
    if (!input || !measurement || !output) throw new Error('Usage: node tools/Select-LegoMeasurement.mjs frames-raw.csv measurement.json frames.csv');
    const issues = [];
    const rows = parseCsv(fs.readFileSync(input, 'utf8'), message => issues.push(message));
    const result = selectMeasurement(rows, JSON.parse(fs.readFileSync(measurement, 'utf8').replace(/^\uFEFF/, '')));
    const headers = Object.keys(result.rows[0]);
    fs.writeFileSync(output, [headers.map(csv).join(','), ...result.rows.map(row => headers.map(key => csv(row[key])).join(','))].join('\n') + '\n');
    fs.writeFileSync(output.replace(/\.csv$/i, '') + '.window.json', JSON.stringify({...result.metadata, measurement_file: path.resolve(measurement), malformed_csv_records: issues.length, parse_warnings: issues.slice(0,20)}, null, 2));
    console.log(`Selected ${result.rows.length} presentation intervals within ${result.metadata.window_seconds.toFixed(3)} measured seconds.`);
  } catch (e) { console.error(e.message); process.exitCode = 1; }
}
