import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("gpu_trace", Path(__file__).with_name("Analyze-LegoGpuTrace.py"))
gpu = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gpu)


class TraceTests(unittest.TestCase):
    def test_union_and_overlap_do_not_double_count_nested_spans(self):
        self.assertEqual(gpu.merge([(0, 10), (2, 4), (8, 15), (20, 20)]), [(0, 15)])
        self.assertEqual(gpu.intersect([(0, 10), (20, 30)], [(5, 25)]), [(5, 10), (20, 25)])

    def test_qpc_alignment_pid_filter_and_count_validation(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            run = root / "llm" / "one"
            run.mkdir(parents=True)
            (root / "experiment.json").write_text(json.dumps({"game_process_id": 42, "mode": "InterFrame"}))
            (run / "measurement.json").write_text(json.dumps({"complete": True, "start_qpc_ticks": "1000", "end_qpc_ticks": "2000", "qpc_frequency": 1000}))
            (run / "summary.json").write_text(json.dumps({"actions": 1, "total_chunks": 1}))
            (run / "calibration-initial.csv").write_text("phase_node,observed_max_host_ms,samples\ndecode:node,100,2\n")
            c = sqlite3.connect(root / "gpu-timeline.sqlite")
            c.executescript('''
                create table TARGET_INFO_SESSION_START_TIME(systemClockNs integer);
                insert into TARGET_INFO_SESSION_START_TIME values(0);
                create table StringIds(id integer, value text);
                insert into StringIds values(1, 'IDXGISwapChain::Present');
                create table NVTX_EVENTS(start integer,end integer,text text,globalTid integer,textId integer);
                create table CUPTI_ACTIVITY_KIND_KERNEL(start integer,end integer,globalPid integer);
                create table DX12_WORKLOAD(start integer,end integer,globalTid integer);
                create table DXGI_API(start integer,end integer,globalTid integer,nameId integer);
            ''')
            tid = (42 << 24) + 7
            for a, b, text in [(1100000000, 1900000000, "action:inference"), (1100000000, 1800000000, "decode:1"), (1200000000, 1300000000, "chunk:node")]:
                c.execute("insert into NVTX_EVENTS values(?,?,?,?,null)", (a, b, text, tid))
            c.execute("insert into CUPTI_ACTIVITY_KIND_KERNEL values(?,?,?)", (1220000000, 1250000000, tid))
            c.execute("insert into CUPTI_ACTIVITY_KIND_KERNEL values(?,?,?)", (1100000000, 1900000000, 99 << 24))
            c.execute("insert into DX12_WORKLOAD values(?,?,?)", (1240000000, 1260000000, tid))
            for a in [1200000000, 1400000000]:
                c.execute("insert into DXGI_API values(?,?,?,1)", (a, a+100, tid))
            c.commit()
            c.close()
            result, _ = gpu.analyze(root)
            self.assertEqual(result["cuda_kernel_union_ms"], 30)
            self.assertEqual(result["dx12_workload_union_ms"], 20)
            self.assertEqual(result["overlapping_recorded_gpu_spans_ms"], 10)
            self.assertEqual(result["complete_nvtx_actions"], 1)
            self.assertEqual(result["chunk_gpu_kernel_union_ms"]["mean"], 30)
            self.assertEqual(result["present_api_rate_fps"], 5)
            (run / "summary.json").write_text(json.dumps({"actions": 2, "total_chunks": 1}))
            with self.assertRaisesRegex(ValueError, 'counts do not match'):
                gpu.analyze(root)


if __name__ == '__main__':
    unittest.main()
