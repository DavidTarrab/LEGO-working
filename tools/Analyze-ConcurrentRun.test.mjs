import test from 'node:test';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import os from 'node:os';
import path from 'node:path';
import { parseCsv, stats, gpuTimestamp, envelope, loadFrames, analyze, writeReport } from './Analyze-ConcurrentRun.mjs';

test('CSV handles BOM, CRLF, escaped quotes and embedded commas/newlines', () => {
  assert.deepEqual(parseCsv('\uFEFFa,b\r\n"hello, ""world""","two\nlines"\r\n'), [{ a: 'hello, "world"', b: 'two\nlines' }]);
  assert.throws(() => parseCsv('a,b\n1'), /expected 2/);
  const issues = [];
  assert.deepEqual(parseCsv('a,b\n1,2\n3', message => issues.push(message)), [{ a: '1', b: '2' }]);
  assert.equal(issues.length, 1);
});

test('nearest-rank tails, invalid samples and explicit timestamp offsets', () => {
  const s = stats([...Array.from({ length: 100 }, (_, i) => i + 1), NaN]);
  assert.equal(s.p50, 50); assert.equal(s.p95, 95); assert.equal(s.p99, 99);
  assert.equal(stats([]).mean, null);
  assert.equal(gpuTimestamp('2026/10/19 10:57:18.512', '-04:00'), Date.parse('2026-10-19T14:57:18.512Z'));
});

test('long-chart reduction preserves a narrow isolated spike and trough', () => {
  const points = Array.from({ length: 10000 }, (_, x) => ({ x, y: x === 6001 ? 999 : x === 6002 ? -2 : 10 }));
  const reduced = envelope(points, 100);
  assert.ok(reduced.length <= 400);
  assert.ok(reduced.some(p => p.y === 999)); assert.ok(reduced.some(p => p.y === -2));
});

function fixture(t) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), 'lego-analysis-'));
  t.after(() => fs.rmSync(root, { recursive: true, force: true }));
  const llm = path.join(root, 'llm', 'timestamp'); fs.mkdirSync(llm, { recursive: true });
  fs.writeFileSync(path.join(llm, 'config.json'), JSON.stringify({ requested_apm: 200, tokens: 16, prompt: '<script>alert(1)</script>' }));
  fs.writeFileSync(path.join(llm, 'summary.json'), JSON.stringify({ actions: 2, elapsed_seconds: 1, skipped_arrivals: 1 }));
  fs.writeFileSync(path.join(llm, 'llm.csv'), 'action,start_utc,latency_ms,deadline_ms,prompt_ms,decode_ms,generated_tokens,prompt_tokens,decode_tokens_per_second\n1,2026-10-19T14:00:00Z,100,300,10,80,16,36,200\n2,2026-10-19T14:00:00.300Z,400,300,20,320,16,36,50\n');
  fs.writeFileSync(path.join(llm, 'gpu.csv'), 'timestamp,index,utilization.gpu [%],memory.used [MiB],memory.total [MiB],power.draw [W]\n2026/10/19 10:00:00.000,0,50 %,6000 MiB,16000 MiB,100 W\n2026/10/19 10:00:00.500,0,80 %,6500 MiB,16000 MiB,150 W\n2026/10/19 10:00:01.000,0,8');
  fs.writeFileSync(path.join(root, 'frames.csv'), 'Application,ProcessID,SwapChainAddress,TimeInSeconds,MsBetweenPresents,Dropped\nGame.exe,1,0x1,1,10,0\nGame.exe,1,0x1,1.02,20,1\nGame.exe,1,0x1,1.05,30,0\nOther.exe,2,0x2,1,999,0\n');
  return root;
}

test('frame stream selection avoids mixing applications and swapchains', t => {
  const root = fixture(t), warnings = [];
  const frame = loadFrames(path.join(root, 'frames.csv'), { fps: 60 }, warnings);
  assert.equal(frame.metrics.valid_frames, 3);
  assert.equal(frame.metrics.interval_ms.max, 30);
  assert.equal(frame.metrics.dropped_presents, 1);
  assert.equal(frame.metrics.over_budget_percent, 200 / 3);
  assert.ok(warnings.some(w => w.includes('selected largest')));
  assert.throws(() => loadFrames(path.join(root, 'frames.csv'), { fps: 60, pid: '99' }, []), /No matching/);
});

test('end-to-end report has correct throughput, deadlines, weighted rates and exports', t => {
  const root = fixture(t);
  const data = analyze({ run: root, fps: 60, gpuUtcOffset: '-04:00', llmBaseline: path.join(root, 'llm') });
  assert.equal(data.metrics.llm.achieved_apm, 120);
  assert.equal(data.metrics.llm.deadline_misses, 1);
  assert.equal(data.metrics.llm.deadline_miss_percent, 50);
  assert.equal(data.metrics.llm.weighted_decode_tokens_per_second, 80);
  assert.equal(data.metrics.gpu[0].minimum_free_memory_mib, 9500);
  assert.equal(data.gpu.rows[0].t, 0);
  assert.ok(data.metrics.warnings.some(w => w.includes('skipped malformed record')));
  assert.equal(data.metrics.comparison[0].change_percent, 0);
  const out = path.join(root, 'analysis'); writeReport(data, out);
  assert.ok(fs.existsSync(path.join(out, 'charts', 'frame-timeline.svg')));
  assert.equal(JSON.parse(fs.readFileSync(path.join(out, 'metrics.json'))).llm.actions, 2);
  const html = fs.readFileSync(path.join(out, 'report.html'), 'utf8');
  assert.ok(html.includes('separate time origins'));
  assert.ok(!html.includes('<script>'));
});

test('missing optional captures degrade gracefully; ambiguous LLM capture fails', t => {
  const root = fixture(t);
  fs.unlinkSync(path.join(root, 'frames.csv'));
  fs.unlinkSync(path.join(root, 'llm', 'timestamp', 'gpu.csv'));
  const data = analyze({ run: root, fps: 60, gpuUtcOffset: '-04:00' });
  assert.equal(data.metrics.frames, null); assert.equal(data.metrics.gpu, null);
  writeReport(data, path.join(root, 'analysis'));
  fs.mkdirSync(path.join(root, 'llm', 'second'));
  fs.copyFileSync(path.join(root, 'llm', 'timestamp', 'llm.csv'), path.join(root, 'llm', 'second', 'llm.csv'));
  assert.throws(() => analyze({ run: root, fps: 60 }), /found 2/);
});

test('mismatched summary uses CSV observation duration and does not invent skipped counts', t => {
  const root = fixture(t);
  fs.writeFileSync(path.join(root, 'llm', 'timestamp', 'summary.json'), JSON.stringify({ actions: 99, elapsed_seconds: 1 }));
  const data = analyze({ run: root, fps: 60, gpuUtcOffset: '-04:00' });
  assert.equal(data.metrics.llm.skipped_arrivals, null);
  assert.ok(Math.abs(data.metrics.llm.elapsed_seconds - .7) < .0001);
  assert.ok(data.metrics.warnings.some(w => w.includes('does not match')));
});

test('integrated timings preserve token accounting, measurement provenance and sampled chunks', t => {
  const root = fixture(t), dir = path.join(root, 'llm', 'timestamp');
  fs.writeFileSync(path.join(dir, 'config.json'), JSON.stringify({timing_source:'integrated-host-wall', latency_origin:'scheduled-arrival', chunk_sampling:'reservoir'}));
  fs.writeFileSync(path.join(dir, 'summary.json'), JSON.stringify({actions:2, elapsed_seconds:1, total_chunks:1000, dropped_chunk_records:998}));
  fs.writeFileSync(path.join(dir, 'llm.csv'), 'action,start_utc,latency_ms,deadline_ms,prompt_ms,decode_ms,generated_tokens,prompt_tokens,decode_tokens_per_second,service_ms,admission_wait_ms,arrival_lateness_ms\n1,2026-10-19T14:00:00Z,200,300,50,100,3,36,20,190,70,10\n2,2026-10-19T14:00:00.300Z,400,300,60,200,5,36,20,380,100,20\n');
  fs.writeFileSync(path.join(dir, 'chunks.csv'), 'submit_elapsed_ms,host_completion_ms\n10,0.2\n700,0.4\n');
  fs.writeFileSync(path.join(root, 'frames.window.json'), JSON.stringify({alignment:'QPC',measurement_file:path.join(dir,'measurement.json')}));
  fs.writeFileSync(path.join(root, 'experiment.json'), JSON.stringify({purpose:'functional validation',background_compilers:[{Id:123}]}));
  const data = analyze({run:root,fps:60,gpuUtcOffset:'-04:00'});
  assert.equal(data.metrics.llm.generated_tokens_per_decode_second,20);
  assert.equal(data.metrics.llm.http_and_other_overhead_ms,null);
  assert.equal(data.metrics.llm.admission_wait_ms.p95,100);
  assert.equal(data.metrics.llm.arrival_lateness_ms.p95,20);
  assert.equal(data.metrics.chunks.omitted_chunks,998);
  assert.equal(data.metrics.inputs.shared_measurement_window,true);
  assert.match(data.metrics.methodology.decode_rate,/first output token/);
  assert.ok(data.metrics.warnings.some(w=>w.includes('CPU contention')));
  assert.ok(data.metrics.warnings.some(w=>w.includes('gaps are not proof')));
  assert.equal(data.frames.rows[0].t,1);
  fs.writeFileSync(path.join(root,'frames.window.json'),JSON.stringify({alignment:'QPC',measurement_file:path.join(root,'other','measurement.json')}));
  const mismatched = analyze({run:root,fps:60,gpuUtcOffset:'-04:00'});
  assert.equal(mismatched.metrics.inputs.shared_measurement_window,false);
  assert.ok(mismatched.metrics.warnings.some(w=>w.includes('provenance does not match')));
});
