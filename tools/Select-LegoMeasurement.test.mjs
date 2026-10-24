import test from 'node:test';
import assert from 'node:assert/strict';
import {selectMeasurement} from './Select-LegoMeasurement.mjs';
test('QPC crop excludes startup and boundary intervals while preserving integer precision', () => {
  const start = 9007199254740993n;
  const rows = [-10, 0, 10, 20, 110].map(offset => ({QPCTime: String(start + BigInt(offset)), ProcessID:'1', SwapChainAddress:'a', MsBetweenPresents:'10'}));
  const result = selectMeasurement(rows, {complete:true, start_qpc_ticks:String(start), end_qpc_ticks:String(start+100n), qpc_frequency:1000});
  assert.equal(result.rows.length,2);
  assert.equal(result.rows[0].TimeInSeconds,'0.010000000');
  assert.equal(result.metadata.outside_window_rows,2);
  assert.equal(result.metadata.first_boundary_rows_dropped,1);
});
test('crop treats swapchains independently and refuses incomplete/empty windows', () => {
  const window = {complete:true,start_qpc_ticks:'100',end_qpc_ticks:'200',qpc_frequency:1000};
  const rows = [
    {QPCTime:'110',ProcessID:'1',SwapChainAddress:'a'}, {QPCTime:'120',ProcessID:'1',SwapChainAddress:'b'},
    {QPCTime:'130',ProcessID:'1',SwapChainAddress:'a'}, {QPCTime:'140',ProcessID:'1',SwapChainAddress:'b'}];
  assert.equal(selectMeasurement(rows,window).metadata.first_boundary_rows_dropped,2);
  assert.throws(() => selectMeasurement(rows,{...window,complete:false}), /incomplete/);
  assert.throws(() => selectMeasurement([{TimeInSeconds:'1'}],window), /No complete/);
});
