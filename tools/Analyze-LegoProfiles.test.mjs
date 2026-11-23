import test from 'node:test';
import assert from 'node:assert/strict';
import {summarizeAdmission} from './Analyze-LegoProfiles.mjs';
function fixture(){
 const data={blocked_polls:4,admission_calls:100,blocked_calls:3,sleep_total_ms:12,sleep_max_ms:8,
  sleep_le_0_25_ms:1,sleep_le_0_5_ms:1,sleep_le_1_ms:0,sleep_le_2_ms:1,sleep_le_5_ms:0,sleep_gt_5_ms:1};
 for(let i=0;i<16;i++)data[`reason_mask_${i}`]=[2,4,6,8].includes(i)?1:0;
 return Object.entries(data).map(([metric,value])=>({metric,value:String(value)}));
}
test('admission profiles keep overlapping reasons distinct from fit-only polls',()=>{
 const result=summarizeAdmission(fixture());
 assert.equal(result.mean_sleep_ms,3);
 assert.equal(result.sleep_over_1_ms_percent,50);
 assert.equal(result.fit_only_percent,25);
 assert.equal(result.gpu_busy_polls,2);
 assert.equal(result.rhi_busy_polls,2);
 assert.equal(result.frame_blocked_polls,0);
 assert.equal(summarizeAdmission([]),null);
});
test('incomplete admission counter/histogram exports fail validation',()=>{
 const data=fixture();data.find(r=>r.metric==='sleep_gt_5_ms').value='0';
 assert.throws(()=>summarizeAdmission(data),/histogram/);
 const reasons=fixture();reasons.find(r=>r.metric==='reason_mask_8').value='0';
 assert.throws(()=>summarizeAdmission(reasons),/reason counts/);
});
