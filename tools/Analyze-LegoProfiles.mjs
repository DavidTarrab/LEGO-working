#!/usr/bin/env node
import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {parseCsv,stats,gpuTimestamp,loadFrames} from './Analyze-ConcurrentRun.mjs';

const read=file=>JSON.parse(fs.readFileSync(file,'utf8').replace(/^\uFEFF/,''));
const csv=file=>parseCsv(fs.readFileSync(file,'utf8'));
const num=value=>value==null||String(value).trim()===''?NaN:Number(value);
const optionalCsv=file=>fs.existsSync(file)?csv(file):[];
const escape=value=>String(value).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
function trials(root){
 if(fs.existsSync(path.join(root,'experiment.json')))return [root];
 return fs.readdirSync(root,{withFileTypes:true}).filter(e=>e.isDirectory()&&!e.isSymbolicLink()&&!['analysis','userdata','llm'].includes(e.name)).flatMap(e=>trials(path.join(root,e.name)));
}
export function summarizeAdmission(rows){
 const summary=Object.fromEntries(rows.map(r=>[r.metric,num(r.value)]));
 if(!rows.length)return null;
 const polls=summary.blocked_polls;
 const hist=['sleep_le_0_25_ms','sleep_le_0_5_ms','sleep_le_1_ms','sleep_le_2_ms','sleep_le_5_ms','sleep_gt_5_ms'];
 if(hist.reduce((sum,key)=>sum+summary[key],0)!==polls)throw new Error('Admission sleep histogram does not match poll count.');
 if(Array.from({length:16},(_,i)=>summary[`reason_mask_${i}`]).reduce((a,b)=>a+b,0)!==polls)throw new Error('Admission reason counts do not match poll count.');
 const forBit=bit=>Array.from({length:16},(_,mask)=>(mask&bit)?summary[`reason_mask_${mask}`]:0).reduce((a,b)=>a+b,0);
 return {...summary,mean_sleep_ms:polls?summary.sleep_total_ms/polls:null,
   sleep_over_1_ms_percent:polls?100*(summary.sleep_le_2_ms+summary.sleep_le_5_ms+summary.sleep_gt_5_ms)/polls:null,
   fit_only_percent:polls?100*summary.reason_mask_8/polls:null,
   frame_blocked_polls:forBit(1),gpu_busy_polls:forBit(2),rhi_busy_polls:forBit(4),no_fit_polls:forBit(8)};
}
function stackChart(rows,title){
 const width=1080,left=360,right=90,top=65,rowHeight=34,height=top+rows.length*rowHeight+35;
 const max=Math.max(1,...rows.map(r=>r.mean_service_ms??0));
 const scale=(width-left-right)/max;
 const parts=[`<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 ${width} ${height}"><rect width="100%" height="100%" fill="white"/><g font-family="Arial,sans-serif" font-size="12"><text x="16" y="24" font-size="18">${escape(title)}</text><text x="${left}" y="45" fill="#2563eb">Recorded admission wait</text><text x="${left+180}" y="45" fill="#d97706">Other host service time (not pure GPU)</text>`];
 rows.forEach((r,i)=>{const y=top+i*rowHeight,wait=r.mean_wait_ms??0,residual=r.residual_service_ms?.mean??0;
  parts.push(`<text x="${left-8}" y="${y+15}" text-anchor="end">${escape(r.label)}</text><rect x="${left}" y="${y}" width="${wait*scale}" height="21" fill="#2563eb"/><rect x="${left+wait*scale}" y="${y}" width="${residual*scale}" height="21" fill="#d97706"/><text x="${left+(wait+residual)*scale+5}" y="${y+15}">${(wait+residual).toFixed(0)} ms</text>`);
 });
 parts.push('</g></svg>');return parts.join('');
}
export function analyzeProfiles(root){
 root=path.resolve(root);
 return trials(root).map(trial=>{
  const meta=read(path.join(trial,'experiment.json'));
  const result={label:path.relative(root,trial).replaceAll('\\','/'),trial,mode:meta.mode,purpose:meta.purpose,extra_arguments:meta.extra_game_arguments??[]};
  const metricsFile=path.join(trial,'analysis/metrics.json');
  const metrics=fs.existsSync(metricsFile)?read(metricsFile):null;
  let start=-Infinity,end=Infinity;
  if(metrics){
   const found=fs.readdirSync(path.join(trial,'llm'),{withFileTypes:true}).filter(e=>e.isDirectory()&&fs.existsSync(path.join(trial,'llm',e.name,'llm.csv')));
   if(found.length!==1)throw new Error(`Expected one native run: ${trial}`);
   const dir=path.join(trial,'llm',found[0].name),summary=read(path.join(dir,'summary.json'));
   const config=read(path.join(dir,'config.json')),actions=csv(path.join(dir,'llm.csv'));
   start=Date.parse(config.start_utc);end=start+metrics.llm.elapsed_seconds*1000;
   Object.assign(result,{success:summary.success,actions:summary.actions,skips:summary.skipped_arrivals,misses:summary.deadline_misses,
    achieved_apm:metrics.llm.achieved_apm,miss_percent:metrics.llm.deadline_miss_percent,rhi_frame_rate:summary.observed_render_frames/metrics.llm.elapsed_seconds,
    mean_latency_ms:metrics.llm.latency_ms.mean,mean_service_ms:metrics.llm.service_ms.mean,mean_wait_ms:metrics.llm.admission_wait_ms.mean,
    residual_service_ms:stats(actions.map(r=>num(r.service_ms)-num(r.admission_wait_ms))),
    chunks_per_action:summary.total_chunks/summary.actions,in_frame_admissions_percent:100*summary.admissions_during_frame/summary.total_chunks,
    admission:summarizeAdmission(optionalCsv(path.join(dir,'admission-summary.csv'))),
    blocked_wait_ms:stats(optionalCsv(path.join(dir,'admission-waits.csv')).map(r=>num(r.wait_ms)))});
   const initial=csv(path.join(dir,'calibration-initial.csv')),final=csv(path.join(dir,'calibration-final.csv'));
   result.initial_max_estimate_ms=Math.max(...initial.map(r=>num(r.observed_max_host_ms)));
   result.final_max_estimate_ms=Math.max(...final.map(r=>num(r.observed_max_host_ms)));
  }
  const frame=metrics?.frames??(fs.existsSync(path.join(trial,'frames.csv'))?loadFrames(path.join(trial,'frames.csv'),{fps:meta.target_fps},[]).metrics:null);
  result.fps=frame?.average_present_rate_fps??null;result.frame_p99_ms=frame?.interval_ms.p99??null;
  const allFocus=optionalCsv(path.join(trial,'focus.csv'));
  const focus=allFocus.filter(r=>Date.parse(r.time_utc)>=start&&Date.parse(r.time_utc)<=end);
  const leadIn=allFocus.filter(r=>Date.parse(r.time_utc)>=start-2000&&Date.parse(r.time_utc)<start);
  result.foreground_lead_in_samples=leadIn.length;
  result.foreground_lead_in_percent=meta.mode!=='LlmOnly'&&leadIn.length?100*leadIn.filter(r=>r.game_is_foreground.toLowerCase()==='true').length/leadIn.length:null;
  result.focus_samples=focus.length;result.focus_scope=metrics?'inference measurement window':'whole capture';
  result.foreground_percent=meta.mode!=='LlmOnly'&&focus.length?100*focus.filter(r=>r.game_is_foreground.toLowerCase()==='true').length/focus.length:null;
  const gpu=optionalCsv(path.join(trial,'gpu.csv')).filter(r=>{const t=gpuTimestamp(r.timestamp,meta.gpu_utc_offset);return t>=start&&t<=end;});
  result.gpu_sm_mhz=stats(gpu.map(r=>num(r['clocks.sm [MHz]'])));result.gpu_temp_c=stats(gpu.map(r=>num(r['temperature.gpu [C]'])));
  return result;
 });
}
export function writeProfileReport(rows,output){
 fs.mkdirSync(output,{recursive:true});
 fs.writeFileSync(path.join(output,'profile.json'),JSON.stringify(rows,null,2));
 const fmt=value=>Number.isFinite(value)?value.toFixed(2):'—';
 const lines=['# Scheduling investigation measurements','','Host duration decomposition includes admission wait and all remaining host service time. The residual is not pure GPU compute. Foreground is sampled at approximately 1 Hz, not continuously. Instrumented traces/CSV logging can perturb timings.','','| Trial | APM | Miss % | Skips | Latency mean ms | Wait mean ms | Residual mean ms | FPS | Frame p99 ms | Foreground % |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |'];
 for(const r of rows)lines.push(`| ${r.label} | ${fmt(r.achieved_apm)} | ${fmt(r.miss_percent)} | ${r.skips??'—'} | ${fmt(r.mean_latency_ms)} | ${fmt(r.mean_wait_ms)} | ${fmt(r.residual_service_ms?.mean)} | ${fmt(r.fps)} | ${fmt(r.frame_p99_ms)} | ${fmt(r.foreground_percent)} |`);
 lines.push('','## Admission profiles','','A blocked poll can have several reasons at once. Fit-only counts polls where no other recorded condition blocks. Sleep is measured host elapsed time around condition-variable wait/reacquisition, not the duration of a GPU kernel.','','| Trial | Requested poll us | Mean actual sleep ms | Sleep >1ms % | Fit-only polls % | Calls blocked % | Inside-frame admits % | GPU SM mean MHz |', '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |');
 for(const r of rows.filter(r=>r.admission)){const a=r.admission;lines.push(`| ${r.label} | ${a.poll_requested_us} | ${fmt(a.mean_sleep_ms)} | ${fmt(a.sleep_over_1_ms_percent)} | ${fmt(a.fit_only_percent)} | ${fmt(100*a.blocked_calls/a.admission_calls)} | ${fmt(r.in_frame_admissions_percent)} | ${fmt(r.gpu_sm_mhz.mean)} |`);}
 lines.push('','## Host service charts','');
 for(const [name,title,predicate]of [['background','Background/observation repeats',r=>r.label.startsWith('baseline-repeats/')],['targeted','Targeted controls and instrumented runs',r=>!r.label.startsWith('baseline-repeats/')]]){
  const selected=rows.filter(r=>r.mean_service_ms!=null&&predicate(r));if(!selected.length)continue;
  fs.writeFileSync(path.join(output,name+'.svg'),stackChart(selected,title));lines.push(`![${title}](${name}.svg)`,'');
 }
 lines.push('A missed 400 ms action can cause the following arrival to be skipped under the existing single-request pacing rule. All comparisons are short, sequential measurements; calibration, phase, temperature, clocks, focus and profiling overhead can vary. Initial historical runs without focus telemetry cannot be retroactively classified.');
 fs.writeFileSync(path.join(output,'profile.md'),lines.join('\n')+'\n');
}
if(process.argv[1]&&path.resolve(process.argv[1])===fileURLToPath(import.meta.url)){
 try{const root=process.argv[2];if(!root)throw new Error('Usage: node tools/Analyze-LegoProfiles.mjs INVESTIGATION_ROOT');const rows=analyzeProfiles(root);const output=path.join(path.resolve(root),'analysis');writeProfileReport(rows,output);console.log(`Profile report: ${path.join(output,'profile.md')}`);}catch(e){console.error(e.message);process.exitCode=1;}
}
