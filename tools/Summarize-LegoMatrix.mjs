#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import {loadFrames} from './Analyze-ConcurrentRun.mjs';

const root = path.resolve(process.argv[2] || '');
const json = file => JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
const status = json(path.join(root,'status.json'));
const rows = status.completed.map(record => {
  const metadata = json(path.join(record.trial,'experiment.json'));
  const metricsFile = path.join(record.trial,'analysis','metrics.json');
  const metrics = fs.existsSync(metricsFile) ? json(metricsFile) : null;
  const frame = metrics?.frames ?? (record.mode === 'RenderOnly' ? loadFrames(path.join(record.trial,'frames.csv'),{fps:metadata.target_fps},[])?.metrics : null);
  return {repetition:record.repetition,mode:record.mode,actions:metrics?.llm.actions ?? null,achieved_apm:metrics?.llm.achieved_apm ?? null,
    deadline_miss_percent:metrics?.llm.deadline_miss_percent ?? null,skipped_arrivals:metrics?.llm.skipped_arrivals ?? null,
    latency_p95_ms:metrics?.llm.latency_ms.p95 ?? null,admission_wait_p95_ms:metrics?.llm.admission_wait_ms.p95 ?? null,
    present_rate_fps:frame?.average_present_rate_fps ?? null,frame_p99_ms:frame?.interval_ms.p99 ?? null,
    over_1_5_budget_percent:frame?.over_1_5_budget_percent ?? null,trial:record.trial};
});
fs.writeFileSync(path.join(root,'comparison.json'),JSON.stringify({purpose:status.purpose,rows},null,2));
const headers = Object.keys(rows[0] || {}), quote = x => '"'+String(x ?? '').replaceAll('"','""')+'"';
fs.writeFileSync(path.join(root,'comparison.csv'),[headers.map(quote).join(','),...rows.map(row=>headers.map(k=>quote(row[k])).join(','))].join('\n')+'\n');
const fmt = x => x == null ? '—' : x.toFixed(2);
const lines = ['# Packaged LEGO comparison','',`Purpose: ${status.purpose || 'unspecified'}. ${status.seconds ?? '?'} seconds per run at requested ${status.requested_apm ?? '?'} APM and ${status.target_fps ?? '?'} FPS.`,
  '', 'These observations use the full model with identical one-node checkpoints and CUDA graphs disabled. They compare admission policies, not checkpoint overhead against stock llama.cpp. Short runs and window-focus changes limit interpretation; repeat longer runs before drawing performance conclusions.',
  '', '| Repeat | Mode | APM | Miss % | Skipped | LLM p95 ms | Wait p95 ms | Present FPS | Frame p99 ms | >1.5× budget % |',
  '| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |'];
for (const r of rows) lines.push(`| ${r.repetition} | ${r.mode} | ${fmt(r.achieved_apm)} | ${fmt(r.deadline_miss_percent)} | ${r.skipped_arrivals ?? '—'} | ${fmt(r.latency_p95_ms)} | ${fmt(r.admission_wait_p95_ms)} | ${fmt(r.present_rate_fps)} | ${fmt(r.frame_p99_ms)} | ${fmt(r.over_1_5_budget_percent)} |`);
lines.push('', 'Per-trial reports include latency/frame distributions, GPU samples, chunk observations and baseline comparisons. Presentation intervals are not GPU render durations. Host admission waits are included in LLM latency. Skipped arrivals must be assessed alongside completed-action deadline misses.','');
for (const r of rows.filter(r=>r.mode!=='RenderOnly')) lines.push(`- [Repeat ${r.repetition}: ${r.mode}](${path.relative(root,path.join(r.trial,'analysis','report.html')).replaceAll('\\','/')})`);
fs.writeFileSync(path.join(root,'summary.md'),lines.join('\n')+'\n');
console.log(`Matrix summary: ${path.join(root,'summary.md')}`);
