#!/usr/bin/env node
// Dependency-free report generator for Measure-LlmLoad.ps1 + PresentMon captures.
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

export function parseCsv(text, onBadRecord) {
  const rows = []; let row = [], field = '', quoted = false;
  text = text.replace(/^\uFEFF/, '');
  for (let i = 0; i < text.length; i++) {
    const c = text[i];
    if (quoted) {
      if (c === '"' && text[i + 1] === '"') { field += '"'; i++; }
      else if (c === '"') quoted = false;
      else field += c;
    } else if (c === '"' && !field) quoted = true;
    else if (c === ',') { row.push(field.trim()); field = ''; }
    else if (c === '\n') { row.push(field.trim()); if (row.some(Boolean)) rows.push(row); row = []; field = ''; }
    else if (c !== '\r') field += c;
  }
  if (quoted) throw new Error('Unterminated quoted CSV field.');
  row.push(field.trim()); if (row.some(Boolean)) rows.push(row);
  if (!rows.length) return [];
  const headers = rows.shift();
  return rows.flatMap((values, i) => {
    if (values.length !== headers.length) {
      const message = `CSV row ${i + 2}: expected ${headers.length} columns, found ${values.length}.`;
      if (onBadRecord) { onBadRecord(message); return []; }
      throw new Error(message);
    }
    return [Object.fromEntries(headers.map((key, j) => [key, values[j]]))];
  });
}
const number = value => value == null || String(value).trim() === '' ? NaN : Number(String(value).replace(/\s*(MiB|%|W)\s*$/i, ''));
const finite = Number.isFinite;
const sum = values => values.reduce((a, b) => a + b, 0);
const maximum = values => values.reduce((a, b) => Math.max(a, b), -Infinity);
const minimum = values => values.reduce((a, b) => Math.min(a, b), Infinity);
export function stats(values) {
  const s = values.filter(finite).sort((a, b) => a - b);
  if (!s.length) return { count: 0, min: null, mean: null, p50: null, p95: null, p99: null, max: null };
  const q = p => s[Math.max(0, Math.ceil(p * s.length) - 1)];
  return { count: s.length, min: s[0], mean: sum(s) / s.length, p50: q(.5), p95: q(.95), p99: q(.99), max: s.at(-1) };
}
export function gpuTimestamp(value, offset) {
  const iso = String(value).trim().replaceAll('/', '-').replace(' ', 'T');
  return Date.parse(iso + (offset || ''));
}
function readJson(file, optional = false) {
  if (optional && !fs.existsSync(file)) return null;
  return JSON.parse(fs.readFileSync(file, 'utf8').replace(/^\uFEFF/, ''));
}
function readCsv(file, warnings) {
  return parseCsv(fs.readFileSync(file, 'utf8'), warnings ? message => warnings.push(`${file}: skipped malformed record. ${message}`) : undefined);
}
function findLlmDirs(root) {
  if (!fs.existsSync(root)) throw new Error(`Directory not found: ${root}`);
  if (fs.existsSync(path.join(root, 'llm.csv'))) return [root];
  return fs.readdirSync(root, { withFileTypes: true }).filter(e => e.isDirectory() && e.name !== 'analysis' && !e.isSymbolicLink())
    .flatMap(e => findLlmDirs(path.join(root, e.name)));
}
function chooseLlm(root) {
  const found = findLlmDirs(root);
  if (found.length !== 1) throw new Error(`Expected one llm.csv under ${root}; found ${found.length}. Pass a specific timestamp directory using --llm-dir (or a specific baseline directory).`);
  return found[0];
}
export function loadLlm(dir, warnings) {
  const config = readJson(path.join(dir, 'config.json'), true) || {};
  const integrated = config.timing_source === 'integrated-host-wall';
  const recorded = readJson(path.join(dir, 'summary.json'), true);
  const raw = readCsv(path.join(dir, 'llm.csv'));
  const rows = raw.map(r => ({ utc: Date.parse(r.start_utc), latency: number(r.latency_ms), deadline: number(r.deadline_ms),
    prompt: number(r.prompt_ms), decode: number(r.decode_ms), tokens: number(r.generated_tokens), promptTokens: number(r.prompt_tokens),
    decodeRate: number(r.decode_tokens_per_second), service: number(r.service_ms), admissionWait: number(r.admission_wait_ms), lateness: number(r.arrival_lateness_ms) }))
    .filter(r => finite(r.utc) && finite(r.latency) && r.latency >= 0).sort((a, b) => a.utc - b.utc);
  if (!rows.length) throw new Error(`No valid inference rows: ${dir}`);
  if (rows.length !== raw.length) warnings.push(`${dir}: rejected ${raw.length - rows.length} rows with invalid timestamps or latency.`);
  const start = rows[0].utc;
  for (const r of rows) r.t = (r.utc - start) / 1000;
  const observedSeconds = (maximum(rows.map(r => r.utc + r.latency)) - start) / 1000;
  const recordedSeconds = number(recorded?.elapsed_seconds);
  const useRecorded = recorded?.actions === rows.length && recordedSeconds > 0;
  if (recorded && !useRecorded) warnings.push(`${dir}: summary count/duration does not match valid CSV rows; throughput derived from first request to last completion.`);
  if (!recorded) warnings.push(`${dir}: summary.json absent; throughput excludes initial/final idle pacing.`);
  const duration = useRecorded ? recordedSeconds : observedSeconds;
  if (recorded?.success === false) warnings.push(`${dir}: run reports failure/cancellation: ${recorded.error || 'no error text'}. Results are incomplete.`);
  if (integrated) warnings.push('Integrated prompt/decode durations are host wall time including admission waits. They are not pure GPU timings or directly comparable to llama-server timings.');
  const deadlineRows = rows.filter(r => finite(r.deadline) && r.deadline > 0);
  const missed = deadlineRows.filter(r => r.latency > r.deadline).length;
  if (deadlineRows.length !== rows.length) warnings.push(`${dir}: some requests have no valid deadline; miss rate uses only requests with deadlines.`);
  const decodeRows = rows.filter(r => r.decode > 0 && r.tokens >= 0);
  const reportedRateRows = rows.filter(r => r.decode > 0 && finite(r.decodeRate) && r.decodeRate >= 0);
  const metrics = {
    actions: rows.length, elapsed_seconds: duration, throughput_source: useRecorded ? 'summary.json elapsed_seconds' : 'first request to last completion',
    achieved_apm: 60 * rows.length / duration, requested_apm: number(config.requested_apm),
    skipped_arrivals: useRecorded ? (recorded.skipped_arrivals ?? null) : null,
    deadline_evaluable_actions: deadlineRows.length, deadline_misses: missed,
    deadline_miss_percent: deadlineRows.length ? 100 * missed / deadlineRows.length : null,
    latency_ms: stats(rows.map(r => r.latency)), prompt_ms: stats(rows.map(r => r.prompt)), decode_ms: stats(rows.map(r => r.decode)),
    timing_source: config.timing_source || 'server-reported', latency_origin: config.latency_origin || 'request-start',
    service_ms: stats(rows.map(r => r.service)), admission_wait_ms: stats(rows.map(r => r.admissionWait)), arrival_lateness_ms: stats(rows.map(r => r.lateness)),
    generated_tokens: stats(rows.map(r => r.tokens)), prompt_tokens: stats(rows.map(r => r.promptTokens)),
    decode_tokens_per_second: stats(rows.map(r => r.decodeRate)),
    weighted_decode_tokens_per_second: reportedRateRows.length ? sum(reportedRateRows.map(r => r.decodeRate * r.decode)) / sum(reportedRateRows.map(r => r.decode)) : null,
    generated_tokens_per_decode_second: decodeRows.length ? 1000 * sum(decodeRows.map(r => integrated ? Math.max(0,r.tokens-1) : r.tokens)) / sum(decodeRows.map(r => r.decode)) : null,
    http_and_other_overhead_ms: integrated ? null : stats(rows.map(r => r.latency - r.prompt - r.decode)),
    unaccounted_host_time_ms: stats(rows.map(r => r.latency - r.prompt - r.decode)),
    first_request_utc: new Date(start).toISOString(), last_completion_utc: new Date(start + observedSeconds * 1000).toISOString()
  };
  return { dir, config, recorded, integrated, rows, start, metrics };
}
export function loadFrames(file, options, warnings) {
  if (!fs.existsSync(file)) { warnings.push(`No frame CSV found at ${file}; frame charts/comparisons omitted.`); return null; }
  const raw = readCsv(file, warnings);
  if (raw.length && !('MsBetweenPresents' in raw[0])) throw new Error(`${file}: requires PresentMon 1.x columns TimeInSeconds and MsBetweenPresents.`);
  const groups = new Map();
  for (const r of raw) {
    if (options.process && r.Application?.toLowerCase() !== options.process.toLowerCase()) continue;
    if (options.pid && r.ProcessID !== options.pid) continue;
    if (options.swapchain && r.SwapChainAddress?.toLowerCase() !== options.swapchain.toLowerCase()) continue;
    const key = JSON.stringify([r.Application, r.ProcessID, r.SwapChainAddress]);
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(r);
  }
  const sorted = [...groups].sort((a, b) => b[1].length - a[1].length);
  if (!sorted.length) throw new Error(`No matching frame streams in ${file}. Check --process / --pid / --swapchain.`);
  const [stream, selected] = sorted[0];
  if (sorted.length > 1) warnings.push(`${file}: selected largest application/PID/swapchain stream (${selected.length}/${raw.length} rows). Use filters to select another; streams are never combined.`);
  const rows = selected.map(r => ({ t: number(r.TimeInSeconds), ms: number(r.MsBetweenPresents),
    dropped: r.Dropped === '1' || r.Dropped?.toLowerCase() === 'true' }))
    .filter(r => finite(r.t) && finite(r.ms) && r.ms > 0).sort((a, b) => a.t - b.t);
  if (!rows.length) throw new Error(`No valid positive frame intervals: ${file}`);
  if (rows.length !== selected.length) warnings.push(`${file}: excluded ${selected.length - rows.length} nonpositive/invalid frame intervals.`);
  const window = readJson(file.replace(/\.csv$/i,'') + '.window.json', true);
  const origin = window?.alignment ? 0 : rows[0].t; for (const r of rows) r.t -= origin;
  const timing = stats(rows.map(r => r.ms)), budget = 1000 / options.fps;
  return { rows, window, metrics: { stream: JSON.parse(stream), total_csv_rows: raw.length, selected_rows: selected.length,
    valid_frames: rows.length, duration_seconds: rows.at(-1).t, interval_ms: timing,
    average_present_rate_fps: 1000 / timing.mean, p99_derived_fps: 1000 / timing.p99,
    target_fps: options.fps, budget_ms: budget,
    over_budget_percent: 100 * rows.filter(r => r.ms > budget).length / rows.length,
    over_1_5_budget_percent: 100 * rows.filter(r => r.ms > budget * 1.5).length / rows.length,
    over_2_budget_percent: 100 * rows.filter(r => r.ms > budget * 2).length / rows.length,
    dropped_presents: rows.filter(r => r.dropped).length,
    worst_frames: [...rows].sort((a, b) => b.ms - a.ms).slice(0, 10).map(r => ({ capture_seconds: r.t, interval_ms: r.ms })) } };
}
function loadGpu(dir, offset, start, duration, warnings) {
  const file = path.join(dir, 'gpu.csv');
  if (!fs.existsSync(file)) { warnings.push(`No gpu.csv in ${dir}; GPU charts omitted.`); return null; }
  const raw = readCsv(file, warnings);
  let rows = raw.map(r => ({ utc: gpuTimestamp(r.timestamp, offset), index: number(r.index),
    util: number(r['utilization.gpu [%]']), memory: number(r['memory.used [MiB]']),
    total: number(r['memory.total [MiB]']), power: number(r['power.draw [W]']) })).filter(r => finite(r.utc));
  if (rows.length !== raw.length) warnings.push(`${file}: rejected ${raw.length - rows.length} unparseable timestamps.`);
  if (!rows.length) { warnings.push(`${file}: no valid GPU timestamps.`); return null; }
  for (const r of rows) r.t = (r.utc - start) / 1000;
  rows.sort((a, b) => a.utc - b.utc);
  const within = rows.filter(r => r.t >= 0 && r.t <= duration);
  if (within.length !== rows.length) warnings.push(`${file}: excluded ${rows.length-within.length} GPU samples outside the LLM measurement interval (startup/warmup/shutdown).`);
  rows = within;
  if (!rows.length) { warnings.push(`${file}: no GPU samples inside the measurement interval.`); return null; }
  const devices = [...new Set(rows.map(r => r.index))].sort((a, b) => a - b);
  const metrics = devices.map(index => {
    const device = rows.filter(r => r.index === index), allocated = device.filter(r => finite(r.memory) && r.total > 0);
    return { index, samples: device.length, utilization_percent: stats(device.map(r => r.util)),
      memory_used_mib: stats(device.map(r => r.memory)), power_w: stats(device.map(r => r.power)),
      minimum_free_memory_mib: allocated.length ? minimum(allocated.map(r => r.total - r.memory)) : null,
      peak_memory_percent: allocated.length ? maximum(allocated.map(r => 100 * r.memory / r.total)) : null,
      first_sample_relative_seconds: device[0].t, last_sample_relative_seconds: device.at(-1).t,
      sampled_span_seconds: (device.at(-1).utc - device[0].utc) / 1000 };
  });
  if (Math.abs(rows[0].t) > 60) warnings.push(`${file}: GPU starts ${rows[0].t.toFixed(1)}s from the first LLM request; verify --gpu-utc-offset.`);
  for (const device of metrics) if (device.last_sample_relative_seconds < duration - 2 || device.first_sample_relative_seconds > 2)
    warnings.push(`GPU ${device.index} samples cover ${fmt(device.first_sample_relative_seconds)} to ${fmt(device.last_sample_relative_seconds)} seconds relative to the first LLM request, versus ${fmt(duration)}s of inference. GPU statistics describe only the sampled portion.`);
  return { rows, metrics };
}
const escape = s => String(s).replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
const fmt = (n, digits = 2) => finite(n) ? n.toLocaleString('en-US', { maximumFractionDigits: digits }) : 'n/a';
const colors = ['#2563eb', '#d97706', '#0d9488', '#db2777', '#7c3aed'];
// Preserve temporal extrema when reducing long traces; a spike survives in its bucket.
export function envelope(points, buckets = 600) {
  if (points.length <= buckets * 4) return points;
  const out = [], size = Math.ceil(points.length / buckets);
  for (let i = 0; i < points.length; i += size) {
    const slice = points.slice(i, i + size);
    const min = slice.reduce((a, b) => b.y < a.y ? b : a), max = slice.reduce((a, b) => b.y > a.y ? b : a);
    out.push(...[...new Set([slice[0], min, max, slice.at(-1)])].sort((a, b) => a.x - b.x));
  }
  return out;
}
function svgChart(title, xLabel, yLabel, series, lines = [], cdf = false) {
  series = series.map(s => ({ ...s, points: s.points.filter(p => finite(p.x) && finite(p.y)) }));
  const points = series.flatMap(s => s.points);
  const width = 1000, height = 340, left = 82, right = 24, top = 62, bottom = 57;
  const xmin = Math.min(0, minimum(points.map(p => p.x))), xmax = Math.max(1, maximum(points.map(p => p.x)));
  const ymax = Math.max(1, maximum(points.map(p => p.y)), maximum(lines.map(l => l.y ?? 0))) * (cdf ? 1 : 1.06);
  const xx = x => left + (x - xmin) / (xmax - xmin) * (width - left - right);
  const yy = y => height - bottom - y / ymax * (height - top - bottom);
  let svg = `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}" role="img" aria-label="${escape(title)}"><rect width="100%" height="100%" fill="white"/><g font-family="Arial,sans-serif" font-size="12" fill="#334155"><text x="${left}" y="23" font-size="17" font-weight="bold">${escape(title)}</text>`;
  let legendX = left;
  for (let i = 0; i < series.length; i++) {
    svg += `<rect x="${legendX}" y="35" width="10" height="10" fill="${colors[i % colors.length]}"/><text x="${legendX + 15}" y="44">${escape(series[i].name)}</text>`;
    legendX += Math.max(140, series[i].name.length * 7 + 35);
  }
  for (let i = 0; i <= 5; i++) {
    const y = ymax * i / 5, x = xmin + (xmax - xmin) * i / 5;
    svg += `<path d="M${left},${yy(y)} H${width - right}" stroke="#e2e8f0"/><text x="${left - 9}" y="${yy(y) + 4}" text-anchor="end">${fmt(y)}</text><text x="${xx(x)}" y="${height - bottom + 20}" text-anchor="middle">${fmt(x)}</text>`;
  }
  for (const l of lines) {
    if (l.y != null) svg += `<path d="M${left},${yy(l.y)} H${width - right}" stroke="#b91c1c" stroke-dasharray="5 4"/><text x="${width - right - 4}" y="${yy(l.y) - 5}" text-anchor="end">${escape(l.label)}</text>`;
  }
  for (let i = 0; i < series.length; i++) {
    const s = series[i], reduced = envelope(s.points);
    svg += `<polyline fill="none" stroke="${colors[i % colors.length]}" stroke-width="1.5" points="${reduced.map(p => `${xx(p.x).toFixed(2)},${yy(p.y).toFixed(2)}`).join(' ')}"/>`;
    if (reduced.length <= 1200) for (const p of reduced) svg += `<circle cx="${xx(p.x)}" cy="${yy(p.y)}" r="2" fill="${colors[i % colors.length]}"><title>${escape(s.name)}: ${fmt(p.x, 3)} ${escape(xLabel)}, ${fmt(p.y, 3)} ${escape(yLabel)}</title></circle>`;
  }
  svg += `<text x="${(left + width - right) / 2}" y="${height - 10}" text-anchor="middle">${escape(xLabel)}</text><text transform="translate(18 ${(top + height - bottom) / 2}) rotate(-90)" text-anchor="middle">${escape(yLabel)}</text></g></svg>`;
  return svg;
}
const series = (name, rows, x, y) => ({ name, points: rows.map(r => ({ x: x(r), y: y(r) })) });
function cdf(name, values) {
  const sorted = values.filter(finite).sort((a, b) => a - b);
  return { name, points: sorted.map((x, i) => ({ x, y: 100 * (i + 1) / sorted.length })) };
}
function completionBins(rows, seconds = 5) {
  const end = maximum(rows.map(r => r.t + r.latency / 1000)), bins = [];
  for (let t = 0; t < end; t += seconds) {
    const span = Math.min(seconds, end - t);
    const count = rows.filter(r => r.t + r.latency / 1000 >= t && r.t + r.latency / 1000 < t + span + (t + span === end ? 1e-9 : 0)).length;
    bins.push({ t: t + span / 2, apm: 60 * count / span });
  }
  return bins;
}
function compareMetric(label, concurrent, baseline, unit, lowerBetter = true) {
  const change = finite(concurrent) && finite(baseline) && baseline !== 0 ? 100 * (concurrent / baseline - 1) : null;
  return { measurement: label, concurrent, baseline, unit, change_percent: change,
    direction: !finite(change) ? 'unavailable' : change === 0 ? 'unchanged' : (change > 0) === lowerBetter ? 'worse' : 'better' };
}
export function analyze(options) {
  const run = path.resolve(options.run), warnings = [];
  const llm = loadLlm(options.llmDir ? path.resolve(options.llmDir) : chooseLlm(path.join(run, 'llm')), warnings);
  const frames = loadFrames(path.join(run, 'frames.csv'), options, warnings);
  const gpu = loadGpu(llm.dir, options.gpuUtcOffset, llm.start, llm.metrics.elapsed_seconds, warnings);
  const baseline = options.llmBaseline ? loadLlm(chooseLlm(path.resolve(options.llmBaseline)), warnings) : null;
  const frameBaseline = options.gameBaseline ? loadFrames(path.join(path.resolve(options.gameBaseline), 'frames.csv'),
    { ...options, pid: undefined, swapchain: undefined }, warnings) : null;
  const experiment = readJson(path.join(run, 'experiment.json'), true);
  const sharedWindow = !!frames?.window?.alignment && !!frames.window.measurement_file &&
    path.resolve(frames.window.measurement_file) === path.join(llm.dir, 'measurement.json');
  if (experiment?.purpose) warnings.push(`Recorded experiment purpose: ${experiment.purpose}.`);
  if (experiment?.background_compilers?.length) warnings.push('Compiler processes were present at launch; CPU contention can confound performance comparisons.');
  if (!options.gpuUtcOffset) warnings.push(`GPU timestamps interpreted in this machine's local timezone (${Intl.DateTimeFormat().resolvedOptions().timeZone}). Use --gpu-utc-offset for portable analysis.`);
  if (sharedWindow) warnings.push('PresentMon frames were selected using this inference run\'s game measurement QPC window. Frame and inference plots share the measurement interval; GPU causality still requires kernel-level traces.');
  else if (frames?.window?.alignment) warnings.push('Frames have a game QPC measurement window, but its provenance does not match this inference run. Cross-stream alignment is unverified.');
  else warnings.push('PresentMon has relative timestamps only. Frame and LLM/GPU plots have separate time origins; no exact cross-stream alignment or causal correlation is inferred.');
  warnings.push('GPU utilization/power samples are coarse device-wide observations; they cannot resolve CUDA layers, kernel overlap, or attribute work to one process.');
  const utrace = path.join(run, 'game.utrace');
  if (fs.existsSync(utrace)) warnings.push('game.utrace is inventoried but not decoded. Open it in Unreal Insights for CPU/RHI/GPU event tracks; frame intervals here come from frames.csv.');
  const comparison = [];
  if (baseline) {
    const mismatches = ['prompt', 'tokens', 'requested_apm', 'cache_prompt', 'parallel_requests', 'timing_source', 'latency_origin', 'upstream_commit', 'checkpoint_nodes', 'warmup_actions', 'context_tokens', 'model'].filter(k => JSON.stringify(llm.config[k]) !== JSON.stringify(baseline.config[k]));
    if (mismatches.length) warnings.push(`LLM baseline settings differ: ${mismatches.join(', ')}. Comparisons are not controlled.`);
    warnings.push(experiment ? 'Inspect experiment.json and baseline metadata for matching scene, model, graphics settings and launch conditions. Window focus is not monitored.' : 'Model identity, server launch settings, game scene, focus and graphics settings are not recorded by the CSVs; verify they match before interpreting baseline changes.');
    for (const q of ['p50', 'p95', 'p99']) comparison.push(compareMetric(`LLM latency ${q}`, llm.metrics.latency_ms[q], baseline.metrics.latency_ms[q], 'ms'));
    comparison.push(compareMetric('Achieved action rate', llm.metrics.achieved_apm, baseline.metrics.achieved_apm, 'APM', false));
    comparison.push(compareMetric('Weighted decode rate', llm.metrics.weighted_decode_tokens_per_second, baseline.metrics.weighted_decode_tokens_per_second, 'tokens/s', false));
  }
  if (frames && frameBaseline) for (const q of ['p50', 'p95', 'p99']) comparison.push(compareMetric(`Frame interval ${q}`, frames.metrics.interval_ms[q], frameBaseline.metrics.interval_ms[q], 'ms'));
  const findings = [];
  findings.push(`${llm.metrics.actions} actions completed at ${fmt(llm.metrics.achieved_apm)} APM (requested ${fmt(llm.metrics.requested_apm)}); ${fmt(llm.metrics.skipped_arrivals, 0)} skipped arrivals.`);
  findings.push(`LLM end-to-end latency: p50 ${fmt(llm.metrics.latency_ms.p50)} ms, p95 ${fmt(llm.metrics.latency_ms.p95)} ms, p99 ${fmt(llm.metrics.latency_ms.p99)} ms. Deadline misses: ${llm.metrics.deadline_misses}/${llm.metrics.deadline_evaluable_actions} (${fmt(llm.metrics.deadline_miss_percent)}%).`);
  if (llm.integrated) findings.push(`Host service time p95 ${fmt(llm.metrics.service_ms.p95)} ms; admission wait p95 ${fmt(llm.metrics.admission_wait_ms.p95)} ms. Admission waits are included in prompt/decode times.`);
  const chunksFile = path.join(llm.dir,'chunks.csv');
  const chunkRows = fs.existsSync(chunksFile) ? readCsv(chunksFile,warnings).map(r => ({t:number(r.submit_elapsed_ms)/1000, ms:number(r.host_completion_ms)})).filter(r => finite(r.t) && finite(r.ms) && r.ms >= 0).sort((a,b) => a.t-b.t) : [];
  const chunkMetrics = chunkRows.length ? {retained_chunks:chunkRows.length, total_chunks:llm.recorded?.total_chunks ?? chunkRows.length,
    omitted_chunks:llm.recorded?.dropped_chunk_records ?? 0, host_completion_ms:stats(chunkRows.map(r=>r.ms)), sampling:llm.config.chunk_sampling ?? 'unspecified',
    admissions_during_frame:llm.recorded?.admissions_during_frame ?? null, admissions_with_gpu_pending:llm.recorded?.admissions_with_gpu_pending ?? null,
    admissions_with_rhi_pending:llm.recorded?.admissions_with_rhi_pending ?? null} : null;
  if (chunkMetrics) {
    findings.push(`Retained ${chunkMetrics.retained_chunks} of ${chunkMetrics.total_chunks} inference chunks; sampled host completion p99 ${fmt(chunkMetrics.host_completion_ms.p99)} ms. GPU-pending admission snapshots: ${fmt(chunkMetrics.admissions_with_gpu_pending,0)}.`);
    warnings.push('Chunk durations are host submission-to-observed-completion times. Admission snapshots can race with newly arriving rendering work; they do not establish kernel overlap or a scheduling violation.');
    if (chunkMetrics.omitted_chunks) warnings.push(`Chunk timeline is sampled (${chunkMetrics.sampling}); its gaps are not proof of idle GPU time.`);
  }
  if (frames) {
    const m = frames.metrics;
    findings.push(`Presentation intervals: p50 ${fmt(m.interval_ms.p50)} ms, p95 ${fmt(m.interval_ms.p95)} ms, p99 ${fmt(m.interval_ms.p99)} ms; worst ${fmt(m.interval_ms.max)} ms. ${fmt(m.over_1_5_budget_percent)}% exceed 1.5× the ${fmt(m.budget_ms)} ms frame budget.`);
    findings.push(`${fmt(m.over_budget_percent)}% exceed the exact frame budget; at a capped frame rate, small pacing jitter can cross this threshold. It is not a count of confirmed visible stutters.`);
  }
  if (gpu) for (const m of gpu.metrics) {
    findings.push(`GPU ${m.index}: utilization mean ${fmt(m.utilization_percent.mean)}%, peak ${fmt(m.utilization_percent.max)}%; peak allocation ${fmt(m.memory_used_mib.max)} MiB; minimum sampled free VRAM ${fmt(m.minimum_free_memory_mib)} MiB; peak power ${fmt(m.power_w.max)} W.`);
    if (m.peak_memory_percent > 90) warnings.push(`GPU ${m.index} exceeds 90% sampled VRAM allocation; check for memory pressure. These counters do not establish shared-memory spill.`);
  }
  for (const c of comparison.filter(c => c.measurement.includes('p99'))) findings.push(`${c.measurement} changes ${fmt(c.change_percent)}% versus baseline (${fmt(c.baseline)} → ${fmt(c.concurrent)} ${c.unit}). One run does not establish statistical significance.`);
  const metrics = { schema_version: 2, generated_utc: new Date().toISOString(), run_directory: run,
    inputs: { llm_directory: llm.dir, llm_baseline: baseline?.dir ?? null, game_baseline: options.gameBaseline ?? null,
      utrace_present: fs.existsSync(utrace), gpu_utc_offset: options.gpuUtcOffset || 'host local timezone', experiment, shared_measurement_window: sharedWindow },
    methodology: { percentile: 'nearest rank: sorted[ceil(p*n)-1]', frame_measure: 'MsBetweenPresents: application presentation intervals, not pure GPU render time',
      frame_time_origin: frames?.window?.alignment ? 'game measurement start (QPC window)' : 'first valid selected present', llm_gpu_time_origin: 'first LLM request UTC',
      frame_budget_ms: 1000 / options.fps, decode_rate: llm.integrated ? 'Host decode rate weighted by decode wall duration, including admission waits. The first output token comes from prefill; decode token count is max(0, generated_tokens - 1).' : 'server-reported decode rate weighted by decode duration; generated_tokens_per_decode_second separately divides output token counts by decode duration (token accounting may differ)',
      chart_reduction: 'temporal bucket first/min/max/last; statistics always use all valid rows' },
    llm: llm.metrics, frames: frames?.metrics ?? null, gpu: gpu?.metrics ?? null, chunks:chunkMetrics,
    baselines: { llm: baseline?.metrics ?? null, frames: frameBaseline?.metrics ?? null }, comparison, findings, warnings };
  return { metrics, llm, frames, gpu, baseline, frameBaseline, chunkRows };
}
export function writeReport(data, output) {
  const { metrics: m, llm, frames, gpu, baseline, frameBaseline, chunkRows } = data;
  fs.mkdirSync(output, { recursive: true }); fs.mkdirSync(path.join(output, 'charts'), { recursive: true });
  const charts = [];
  function chart(name, title, x, y, data, lines = [], isCdf = false, note = '') {
    const svg = svgChart(title, x, y, data, lines, isCdf);
    fs.writeFileSync(path.join(output, 'charts', `${name}.svg`), svg);
    charts.push({ name, title, svg, note });
  }
  if (frames) {
    chart('frame-timeline', 'Presentation intervals over time', frames.window ? 'Seconds from measurement start' : 'Seconds from first selected present', 'Interval (ms)',
      [series('Concurrent', frames.rows, r => r.t, r => r.ms)], [{ y: m.frames.budget_ms, label: `${fmt(m.frames.target_fps)} FPS budget` }], false,
      frames.window ? 'Shared game QPC measurement window; startup and warmup excluded.' : 'Separate frame-capture clock. Temporal reduction preserves bucket minima and maxima, including spikes.');
    chart('frame-cdf', 'Frame interval distribution', 'Presentation interval (ms)', 'Cumulative percent',
      [cdf('Concurrent', frames.rows.map(r => r.ms)), ...(frameBaseline ? [cdf('Game only', frameBaseline.rows.map(r => r.ms))] : [])], [], true,
      'Lower intervals are faster. This measures application presents, including those marked dropped.');
  }
  chart('llm-timeline', 'End-to-end inference latency', 'Seconds from first LLM request', 'Latency (ms)',
    [series('Concurrent', llm.rows, r => r.t, r => r.latency), series('Request deadline', llm.rows, r => r.t, r => r.deadline)]);
  chart('llm-cdf', 'Inference latency distribution', 'End-to-end latency (ms)', 'Cumulative percent',
    [cdf('Concurrent', llm.rows.map(r => r.latency)), ...(baseline ? [cdf('LLM only', baseline.rows.map(r => r.latency))] : [])], [], true);
  chart('llm-components', llm.integrated ? 'Host prompt and decode times' : 'Server prompt and decode times', 'Seconds from first LLM request', llm.integrated ? 'Host duration (ms)' : 'Server duration (ms)',
    [series('Prompt', llm.rows, r => r.t, r => r.prompt), series('Decode', llm.rows, r => r.t, r => r.decode)], [], false,
    llm.integrated ? 'Host durations include admission waits and synchronization. Neither component is a pure GPU kernel measurement.' : 'Server timings exclude HTTP and other request overhead; neither component is a pure GPU kernel measurement.');
  if (llm.integrated) chart('admission-wait','Inference admission waits','Seconds from measurement start','Wait (ms)',[series('Admission wait',llm.rows,r=>r.t,r=>r.admissionWait)]);
  if (chunkRows?.length) chart('inference-chunks','Observed inference chunk completion times','Seconds from measurement start','Host duration (ms)',[series('Retained chunks',chunkRows,r=>r.t,r=>r.ms)],[],false,'Host observations; timeline may be reservoir-sampled. See sampling and total counts in metrics.json.');
  chart('throughput', 'Completed actions per minute in 5-second bins', 'Seconds from first LLM request', 'Completion rate (APM)',
    [series('Concurrent', completionBins(llm.rows), r => r.t, r => r.apm)],
    finite(m.llm.requested_apm) ? [{ y: m.llm.requested_apm, label: 'Requested APM' }] : [], false,
    'Rates count completions. The final bin may be shorter; whole-run achieved APM uses the recorded measurement duration when consistent.');
  if (gpu) for (const [name, field, title, unit] of [['gpu-utilization', 'util', 'GPU utilization', 'Utilization (%)'],
    ['gpu-memory', 'memory', 'GPU allocated memory', 'Memory (MiB)'], ['gpu-power', 'power', 'GPU power draw', 'Power (W)']]) {
    chart(name, title, 'Seconds from first LLM request', unit,
      gpu.metrics.map(device => series(`GPU ${device.index}`, gpu.rows.filter(r => r.index === device.index), r => r.t, r => r[field])), [], false,
      'Coarse device-wide samples aligned to LLM UTC timestamps using the configured GPU timestamp offset.');
  }
  const comparisonRows = m.comparison.map(c => `<tr><td>${escape(c.measurement)}</td><td>${fmt(c.baseline)} ${escape(c.unit)}</td><td>${fmt(c.concurrent)} ${escape(c.unit)}</td><td>${fmt(c.change_percent)}%</td></tr>`).join('');
  const statRow = (name, stat, unit) => `<tr><td>${name}</td>${['mean', 'p50', 'p95', 'p99', 'max'].map(k => `<td>${fmt(stat[k])} ${unit}</td>`).join('')}</tr>`;
  const cards = [['LLM p99', `${fmt(m.llm.latency_ms.p99)} ms`], ['Achieved APM', fmt(m.llm.achieved_apm)],
    ['Deadline misses', `${fmt(m.llm.deadline_miss_percent)}%`], ...(m.frames ? [['Frame p99', `${fmt(m.frames.interval_ms.p99)} ms`]] : [])];
  const html = `<!doctype html><html lang="en"><meta charset="utf-8"><meta name="viewport" content="width=device-width, initial-scale=1"><title>Concurrent GPU workload report</title>
<style>body{font:15px/1.6 system-ui,sans-serif;color:#172033;background:#f1f5f9;margin:0}main{max-width:1120px;margin:auto;padding:32px 22px}h1,h2{line-height:1.25}h1{margin-bottom:8px}a{color:#1d4ed8}section,figure{background:white;padding:22px;border-radius:10px;margin:20px 0;box-shadow:0 1px 5px #0f172a0b}figure{margin-left:0;margin-right:0}figure svg{width:100%;height:auto}figcaption,.muted{color:#526174;font-size:13px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:14px}.card{background:white;padding:18px;border-top:4px solid #2563eb;border-radius:8px}.card strong{display:block;font-size:27px}table{border-collapse:collapse;width:100%;font-variant-numeric:tabular-nums}th,td{text-align:left;border-bottom:1px solid #e2e8f0;padding:10px}code{overflow-wrap:anywhere}.scroll{overflow:auto}@media print{body{background:white}section,figure{box-shadow:none;break-inside:avoid}main{padding:0}}</style>
<main><h1>Concurrent GPU workload report</h1><p class="muted">${escape(m.run_directory)}<br>Generated ${escape(m.generated_utc)} · ${m.llm.actions} requests${m.frames ? ` · ${m.frames.valid_frames} presents` : ''}</p>
<div class="cards">${cards.map(([label, value]) => `<div class="card">${escape(label)}<strong>${escape(value)}</strong></div>`).join('')}</div>
<section><h2>What this run shows</h2><ul>${m.findings.map(f => `<li>${escape(f)}</li>`).join('')}</ul></section>
<section><h2>Measurements</h2><div class="scroll"><table><thead><tr><th>Measurement</th><th>Mean</th><th>p50</th><th>p95</th><th>p99</th><th>Maximum</th></tr></thead><tbody>
${statRow('LLM end-to-end', m.llm.latency_ms, 'ms')}${statRow('Prompt processing', m.llm.prompt_ms, 'ms')}${statRow('Decode', m.llm.decode_ms, 'ms')}${statRow('Generated tokens/request', m.llm.generated_tokens, '')}${m.frames ? statRow('Presentation interval', m.frames.interval_ms, 'ms') : ''}</tbody></table></div>
<p>${llm.integrated ? 'Host-measured decode rate including admission waits' : 'Server-reported decode rate'}, weighted by decode duration: <b>${fmt(m.llm.weighted_decode_tokens_per_second)} tokens/s</b>. ${llm.integrated ? 'The first output token is produced during prompt processing; subsequent output tokens divided by decode duration' : 'Output token counts divided by decode duration'}: ${fmt(m.llm.generated_tokens_per_decode_second)} tokens/s. Percentiles use nearest rank. Request/frame statistics use all valid rows; chunk statistics use the retained sample.</p>
${m.frames ? `<p>Mean present rate: ${fmt(m.frames.average_present_rate_fps)} FPS. 1000 / p99 interval: ${fmt(m.frames.p99_derived_fps)} FPS (a latency-derived measure). Dropped presents: ${m.frames.dropped_presents}. Selected stream: <code>${escape(JSON.stringify(m.frames.stream))}</code>.</p>` : ''}</section>
${comparisonRows ? `<section><h2>Baseline comparison</h2><div class="scroll"><table><tr><th>Measurement</th><th>Baseline</th><th>Concurrent</th><th>Change</th></tr>${comparisonRows}</table></div><p class="muted">Positive change means the numeric value increased. Lower latency is preferable; higher throughput is preferable. Single-run changes are descriptive.</p></section>` : ''}
${charts.map(c => `<figure>${c.svg}<figcaption>${escape(c.note)} <a href="charts/${c.name}.svg" download>Download SVG</a></figcaption></figure>`).join('')}
${m.frames ? `<section><h2>Ten largest frame intervals</h2><table><tr><th>Frame-capture seconds</th><th>Interval (ms)</th></tr>${m.frames.worst_frames.map(r => `<tr><td>${fmt(r.capture_seconds, 3)}</td><td>${fmt(r.interval_ms, 3)}</td></tr>`).join('')}</table></section>` : ''}
<section><h2>Interpretation and data quality</h2><ul>${m.warnings.map(w => `<li>${escape(w)}</li>`).join('')}</ul><p>Methodology and all numeric results: <a href="metrics.json">metrics.json</a>. Text report: <a href="summary.md">summary.md</a>.</p></section></main></html>`;
  fs.writeFileSync(path.join(output, 'report.html'), html);
  fs.writeFileSync(path.join(output, 'metrics.json'), JSON.stringify(m, null, 2) + '\n');
  fs.writeFileSync(path.join(output, 'summary.md'), `# Concurrent GPU workload report\n\nRun: ${m.run_directory}\n\n${m.findings.map(f => `- ${f}`).join('\n')}\n\n## Baseline comparisons\n\n${m.comparison.map(c => `- ${c.measurement}: ${fmt(c.baseline)} → ${fmt(c.concurrent)} ${c.unit}; change ${fmt(c.change_percent)}%.`).join('\n') || 'No comparable baseline supplied.'}\n\n## Interpretation and data quality\n\n${m.warnings.map(w => `- ${w}`).join('\n')}\n`);
}
function main() {
  const help = `Usage: node tools/Analyze-ConcurrentRun.mjs <trial-directory> [options]
  --output <directory>          Default: <trial-directory>/analysis
  --llm-dir <directory>         Exact timestamp folder if trial has multiple LLM runs
  --llm-baseline <directory>    One LLM-only capture (or parent with exactly one capture)
  --game-baseline <directory>   Folder containing game-only frames.csv
  --gpu-utc-offset <±HH:MM>     Offset at capture time, e.g. -04:00 for New York daylight time
  --fps <number>               Frame budget target (default 60)
  --process <exe-name>          Filter frame streams by executable
  --pid <number>               Filter concurrent frame stream by PID
  --swapchain <address>        Filter concurrent frame stream by swapchain
  --help                      Show help
Produces standalone report.html, charts/*.svg, metrics.json and summary.md.
Reads CSV/JSON only; game.utrace must be inspected separately in Unreal Insights.`;
  const args = process.argv.slice(2);
  if (!args.length || args.includes('--help')) { console.log(help); return; }
  const options = { run: args.shift(), fps: 60 };
  const keys = { '--output': 'output', '--llm-dir': 'llmDir', '--llm-baseline': 'llmBaseline', '--game-baseline': 'gameBaseline',
    '--gpu-utc-offset': 'gpuUtcOffset', '--fps': 'fps', '--process': 'process', '--pid': 'pid', '--swapchain': 'swapchain' };
  while (args.length) {
    const flag = args.shift(), key = keys[flag];
    if (!key || !args.length || args[0].startsWith('--')) throw new Error(`Unknown option or missing value: ${flag}\n${help}`);
    options[key] = args.shift();
  }
  options.fps = Number(options.fps);
  if (!finite(options.fps) || options.fps <= 0) throw new Error('--fps must be positive.');
  if (options.gpuUtcOffset && !/^[+-](?:0\d|1[0-4]):[0-5]\d$/.test(options.gpuUtcOffset)) throw new Error('--gpu-utc-offset must have format ±HH:MM.');
  const data = analyze(options), output = path.resolve(options.output || path.join(options.run, 'analysis'));
  writeReport(data, output);
  console.log(`Report: ${path.join(output, 'report.html')}\n\n${data.metrics.findings.join('\n')}\n\nNotes:\n${data.metrics.warnings.map(w => `- ${w}`).join('\n')}`);
}
if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  try { main(); } catch (error) { console.error(`Analysis failed: ${error.message}`); process.exitCode = 1; }
}
