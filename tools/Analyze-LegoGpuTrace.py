"""Analyze a Windows Nsight SQLite export, aligned to LEGO's raw QPC window.

Uses only Python's standard library. Captures are diagnostic: CUDA/NVTX/DX12
tracing can add substantial overhead. Overlapping recorded GPU spans do not
establish simultaneous SM execution or quantify hardware resource sharing.
"""
import argparse
import bisect
import collections
import csv
import json
import math
from pathlib import Path
import sqlite3


def stats(values):
    values = sorted(values)
    if not values:
        return {"count": 0, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    def pct(p):
        return values[max(0, math.ceil(p * len(values)) - 1)]
    return {"count": len(values), "mean": sum(values) / len(values),
            "p50": pct(.5), "p95": pct(.95), "p99": pct(.99), "max": values[-1]}


def merge(intervals):
    result = []
    for a, b in sorted(intervals):
        if b <= a:
            continue
        if result and a <= result[-1][1]:
            result[-1] = (result[-1][0], max(b, result[-1][1]))
        else:
            result.append((a, b))
    return result


def intersect(left, right):
    i = j = 0
    result = []
    while i < len(left) and j < len(right):
        a, b = max(left[i][0], right[j][0]), min(left[i][1], right[j][1])
        if b > a:
            result.append((a, b))
        if left[i][1] < right[j][1]:
            i += 1
        else:
            j += 1
    return result


def read_json(file):
    return json.loads(file.read_text(encoding="utf-8-sig"))


def analyze(trial):
    trial = Path(trial).resolve()
    metadata = read_json(trial / "experiment.json")
    runs = list((trial / "llm").glob("*/measurement.json"))
    if len(runs) != 1:
        raise ValueError("Expected one complete native inference measurement.")
    window = read_json(runs[0])
    if not window.get("complete"):
        raise ValueError("Incomplete measurement window.")
    summary = read_json(runs[0].parent / "summary.json")
    database = trial / "gpu-timeline.sqlite"
    connection = sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)
    tables = {r[0] for r in connection.execute("select name from sqlite_master where type='table'")}
    required = {"TARGET_INFO_SESSION_START_TIME", "CUPTI_ACTIVITY_KIND_KERNEL", "NVTX_EVENTS", "DXGI_API", "DX12_WORKLOAD"}
    if not required.issubset(tables):
        raise ValueError("Trace lacks required CUDA/NVTX/DX12 tables: " + str(required - tables))
    session = connection.execute("select systemClockNs from TARGET_INFO_SESSION_START_TIME").fetchone()
    if not session or session[0] is None:
        raise ValueError("Trace does not provide a Windows QPC session origin.")
    frequency = int(window["qpc_frequency"])
    start = int(window["start_qpc_ticks"]) * 1_000_000_000 // frequency - session[0]
    end = int(window["end_qpc_ticks"]) * 1_000_000_000 // frequency - session[0]
    if end <= start:
        raise ValueError("Invalid QPC window.")
    pid = int(metadata["game_process_id"])
    # Nsight serializes PID in bits 24..47, TID in bits 0..23.
    pid_sql = "((globalTid >> 24) & 16777215) = ?"
    gpu_pid_sql = "((globalPid >> 24) & 16777215) = ?"
    def intervals(table, predicate=pid_sql):
        return [(max(a, start), min(b, end)) for a, b in connection.execute(
            "select start,end from " + table + " where " + predicate + " and end>? and start<? order by start", (pid, start, end))]
    kernels = intervals("CUPTI_ACTIVITY_KIND_KERNEL", gpu_pid_sql)
    graphics = intervals("DX12_WORKLOAD")
    cuda_union, graphics_union = merge(kernels), merge(graphics)
    overlap = intersect(cuda_union, graphics_union)
    busy = merge(cuda_union + graphics_union)
    total_ms = (end - start) / 1e6
    length = lambda spans: sum(b - a for a, b in spans) / 1e6
    nvtx = connection.execute(
        "select n.start,n.end,coalesce(n.text,s.value) from NVTX_EVENTS n left join StringIds s on s.id=n.textId "
        "where " + pid_sql + " and n.start>=? and n.end<=? order by n.start", (pid, start, end)).fetchall()
    actions = [(a, b) for a, b, name in nvtx if name == "action:inference"]
    phases = [(a, b, name.split(':')[0]) for a, b, name in nvtx if name and (name.startswith('prefill:') or name.startswith('decode:'))]
    phase_starts = [r[0] for r in phases]
    kernel_starts = [r[0] for r in kernels]
    chunks, node_groups = [], collections.defaultdict(list)
    for a, b, name in nvtx:
        if not name or not name.startswith("chunk:"):
            continue
        # These kernels are contained in a host observation range. Empty ranges
        # include view/metadata operations; they are not missing GPU samples.
        lo, hi = bisect.bisect_left(kernel_starts, a), bisect.bisect_right(kernel_starts, b)
        contained = [(x, y) for x, y in kernels[lo:hi] if y <= b]
        index = bisect.bisect_right(phase_starts, a) - 1
        phase = phases[index][2] if index >= 0 and phases[index][1] >= b else "unknown"
        row = {"phase_node": phase + ":" + name[6:], "host_ms": (b - a) / 1e6,
               "gpu_kernel_union_ms": length(merge(contained)), "kernel_count": len(contained)}
        chunks.append(row)
        node_groups[row["phase_node"]].append(row)
    presents = connection.execute(
        "select a.start from DXGI_API a join StringIds s on s.id=a.nameId where " + pid_sql +
        " and a.start>=? and a.start<=? and s.value in ('IDXGISwapChain::Present','IDXGISwapChain1::Present1') order by a.start", (pid, start, end)).fetchall()
    present_intervals = [(b[0] - a[0]) / 1e6 for a, b in zip(presents, presents[1:])]
    estimates = {}
    with (runs[0].parent / "calibration-initial.csv").open(encoding="utf-8-sig", newline="") as file:
        estimates = {r["phase_node"]: float(r["observed_max_host_ms"]) for r in csv.DictReader(file)}
    nodes = []
    for name, values in node_groups.items():
        host = stats([v["host_ms"] for v in values])
        gpu = stats([v["gpu_kernel_union_ms"] for v in values])
        nodes.append({"phase_node": name, "host_ms": host, "gpu_kernel_union_ms": gpu,
                      "initial_host_estimate_ms": estimates.get(name),
                      "zero_kernel_ranges": sum(v["kernel_count"] == 0 for v in values)})
    diagnostics = connection.execute("select severity,text from DIAGNOSTIC_EVENT where severity>=2").fetchall() if "DIAGNOSTIC_EVENT" in tables else []
    runtime = []
    if "CUPTI_ACTIVITY_KIND_RUNTIME" in tables:
        runtime = [{"name": name, "count": count, "total_host_ms": ns / 1e6} for name, count, ns in connection.execute(
            "select s.value,count(*),sum(a.end-a.start) from CUPTI_ACTIVITY_KIND_RUNTIME a join StringIds s on s.id=a.nameId where " + pid_sql +
            " and a.start>=? and a.end<=? group by a.nameId order by sum(a.end-a.start) desc limit 15", (pid, start, end))]
    connection.close()
    result = {"mode": metadata["mode"], "seconds": total_ms / 1000, "alignment": "raw Windows QPC ticks to Nsight systemClockNs",
              "actions_logged": summary["actions"], "complete_nvtx_actions": len(actions),
              "chunks_logged": summary["total_chunks"], "complete_nvtx_chunks": len(chunks),
              "cuda_kernel_count": len(kernels), "cuda_kernel_union_ms": length(cuda_union),
              "dx12_workload_union_ms": length(graphics_union), "overlapping_recorded_gpu_spans_ms": length(overlap),
              "neither_recorded_cuda_nor_dx12_ms": total_ms - length(busy),
              "host_chunk_ms": stats([r["host_ms"] for r in chunks]),
              "chunk_gpu_kernel_union_ms": stats([r["gpu_kernel_union_ms"] for r in chunks]),
              "chunks_without_kernels": sum(r["kernel_count"] == 0 for r in chunks),
              "present_api_intervals_ms": stats(present_intervals),
              "present_api_rate_fps": 1000 * len(present_intervals) / sum(present_intervals) if present_intervals else None,
              "cuda_runtime_calls": runtime, "nodes": nodes, "diagnostics": diagnostics,
              "limitations": ["Instrumented diagnostic capture; profiling changes latency and frame pacing.",
                              "GPU span overlap does not establish simultaneous SM execution.",
                              "Uncovered spans may include transfers, other processes, and untraced GPU work; they are not proof of idle hardware.",
                              "Present API calls do not establish successful display or distinguish test calls.",
                              "Host calibration is a high-water observation, not a GPU execution estimate."]}
    if len(actions) != summary["actions"] or len(chunks) != summary["total_chunks"]:
        raise ValueError("NVTX/QPC capture counts do not match the measured actions/chunks.")
    return result, {"cuda": cuda_union, "dx12": graphics_union, "overlap": overlap, "start": start, "end": end}


def timeline_svg(spans):
    # First 500 ms keeps the exported graphic readable. Full trace remains in SQLite.
    start, end = spans["start"], min(spans["end"], spans["start"] + 500_000_000)
    scale = 1100 / (end - start)
    parts = ['<svg xmlns="http://www.w3.org/2000/svg" width="1280" height="220" viewBox="0 0 1280 220"><rect width="1280" height="220" fill="white"/>',
             '<text x="16" y="24" font-family="sans-serif">Recorded GPU spans: first 500 ms of measurement (profiled)</text>']
    for i, (name, color) in enumerate([("cuda", "#2563eb"), ("dx12", "#d97706"), ("overlap", "#9333ea")]):
        y = 55 + i * 45
        parts.append(f'<text x="16" y="{y+16}" font-family="sans-serif">{name}</text>')
        for a, b in spans[name]:
            a, b = max(a, start), min(b, end)
            if b > a:
                parts.append(f'<rect x="{140+(a-start)*scale:.3f}" y="{y}" width="{max(.1,(b-a)*scale):.3f}" height="22" fill="{color}"/>')
    parts.append('</svg>')
    return ''.join(parts)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trial", type=Path)
    args = parser.parse_args()
    result, spans = analyze(args.trial)
    output = args.trial / "gpu-analysis"
    output.mkdir(exist_ok=True)
    (output / "metrics.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    (output / "timeline.svg").write_text(timeline_svg(spans), encoding="utf-8")
    fields = ["mode", "seconds", "complete_nvtx_actions", "cuda_kernel_count", "cuda_kernel_union_ms", "dx12_workload_union_ms", "overlapping_recorded_gpu_spans_ms", "chunks_without_kernels", "present_api_rate_fps"]
    lines = ['# CUDA and graphics diagnostic trace', '', '| Measurement | Value |', '| --- | ---: |']
    lines += [f'| {key} | {result[key]} |' for key in fields]
    lines += ['', '![Recorded spans](timeline.svg)', '', *['- '+v for v in result['limitations']], '']
    (output / "summary.md").write_text('\n'.join(lines), encoding="utf-8")
    print(output / "summary.md")


if __name__ == "__main__":
    main()
