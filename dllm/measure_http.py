"""Paced HTTP requests with QPC arrival deadlines and retained response evidence.

Supports both llama-server and the diffusion server. Optional game window uses
the game's real QPC start; request drain stays in the measured interval. The
orchestrator must keep rendering alive through that drain and verify coverage.
"""
from __future__ import annotations

import argparse
import csv
import ctypes
from datetime import datetime, timedelta, timezone
import json
import math
import os
from pathlib import Path
import time
from urllib.request import Request, urlopen

from profile_dllm import atomic_json, source_manifest
from runtime import VALIDATION_PROMPT


class QpcClock:
    def __init__(self):
        if os.name == "nt":
            self.kernel = ctypes.WinDLL("kernel32", use_last_error=True)
            self.kernel.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
            self.kernel.QueryPerformanceFrequency.argtypes = [ctypes.POINTER(ctypes.c_longlong)]
            frequency = ctypes.c_longlong()
            if not self.kernel.QueryPerformanceFrequency(ctypes.byref(frequency)):
                raise ctypes.WinError(ctypes.get_last_error())
            self.frequency = frequency.value
        else:
            self.frequency = 1_000_000_000

    def now(self):
        if os.name != "nt":
            return time.perf_counter_ns()
        value = ctypes.c_longlong()
        if not self.kernel.QueryPerformanceCounter(ctypes.byref(value)):
            raise ctypes.WinError(ctypes.get_last_error())
        return value.value


def arrival_ticks(index: int, apm: int, frequency: int) -> int:
    return index * 60 * frequency // apm


def http_json(uri, value=None, timeout=300):
    data = None if value is None else json.dumps(value).encode("utf-8")
    with urlopen(Request(uri, data=data, headers={"Content-Type": "application/json"}), timeout=timeout) as response:
        return json.load(response)


def game_origin(path: Path, clock: QpcClock, timeout=120):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            value = json.loads(path.read_text(encoding="utf-8-sig"))
            if value.get("complete"):
                raise ValueError("Game window already complete; refusing a late capture")
            if value["qpc_frequency"] != clock.frequency:
                raise ValueError("Game and client QPC frequencies differ")
            origin = int(value["start_qpc_ticks"])
            if not 0 <= (clock.now() - origin) / clock.frequency <= .25:
                raise ValueError("External client missed the game start by more than 250 ms")
            return origin
        except (FileNotFoundError, json.JSONDecodeError):
            time.sleep(.05)
    raise TimeoutError("Game did not publish a measurement start")


def measure(args):
    args.output_dir.mkdir(parents=True, exist_ok=False)
    status = {"success": None, "actions": 0, "skipped_arrivals": 0}
    atomic_json(args.output_dir / "status.json", status)
    clock = QpcClock()
    try:
        health = http_json(args.server.rstrip("/") + "/health", timeout=15)
        if health.get("status") != "ok":
            raise ValueError("Server not ready")
        body = {"prompt": args.prompt, "n_predict": args.tokens, "temperature": 0,
                "seed": 1, "cache_prompt": False, "stream": False}
        if args.steps is not None:
            body["steps"] = args.steps
        for _ in range(args.warmup):
            http_json(args.server.rstrip("/") + "/completion", body)
        origin = game_origin(args.window_file, clock) if args.window_file else clock.now()
        anchor_qpc = clock.now()
        anchor_utc = datetime.now(timezone.utc)
        utc = anchor_utc - timedelta(seconds=(anchor_qpc - origin) / clock.frequency)
        end = origin + args.seconds * clock.frequency
        timing_source = health.get("timing_source", "server-reported")
        model = health.get("model")
        model_info = model if isinstance(model, dict) else {}
        config = {"baseline": "external-process", "timing_source": timing_source,
                  "model": model_info.get("model_id", model),
                  "model_revision": model_info.get("model_revision"),
                  "quantization": model_info.get("quantization"),
                  "diffusion_steps": body.get("steps", health.get("default_steps")),
                  "skip_layers": health.get("default_skip_layers"),
                  "latency_origin": "scheduled-arrival", "requested_apm": args.apm,
                  "seconds": args.seconds, "tokens": args.tokens, "prompt": args.prompt,
                  "parallel_requests": 1, "cache_prompt": False, "server_health": health,
                  "source": source_manifest(), "start_utc": utc.isoformat(),
                  "warmup_actions": args.warmup, "request": body,
                  "measurement_source": str(args.window_file) if args.window_file else None,
                  "window_rule": "fixed arrival window; finish in-flight request while game remains alive"}
        atomic_json(args.output_dir / "config.json", config)
        window = {"start_qpc_ticks": str(origin), "qpc_frequency": clock.frequency,
                  "start_utc": utc.isoformat(), "complete": False}
        atomic_json(args.output_dir / "measurement.json", window)
        offered = math.ceil(args.seconds * args.apm / 60)
        index, missed, completed, deadline_misses = 0, 0, 0, 0
        last_complete = origin
        with (args.output_dir / "llm.csv").open("x", encoding="utf-8", newline="") as csv_file, \
             (args.output_dir / "responses.jsonl").open("x", encoding="utf-8") as responses, \
             (args.output_dir / "arrivals.jsonl").open("x", encoding="utf-8") as arrivals:
            fields = ["action", "start_utc", "start_elapsed_ms", "latency_ms", "deadline_ms", "deadline_missed",
                      "service_ms", "arrival_lateness_ms", "prompt_tokens", "prompt_ms", "generated_tokens",
                      "decode_ms", "decode_tokens_per_second", "diffusion_steps", "executed_forwards", "admission_wait_ms",
                      "scheduled_qpc_ticks", "dispatch_qpc_ticks", "completion_qpc_ticks"]
            writer = csv.DictWriter(csv_file, fieldnames=fields)
            writer.writeheader()
            while index < offered:
                scheduled = origin + arrival_ticks(index, args.apm, clock.frequency)
                remaining = (scheduled - clock.now()) / clock.frequency
                if remaining > 0:
                    time.sleep(remaining)
                dispatch = clock.now()
                if dispatch >= end:
                    while index < offered:
                        arrivals.write(json.dumps({"arrival": index, "outcome": "skipped-end-window"}) + "\n")
                        index += 1
                        missed += 1
                    break
                response = http_json(args.server.rstrip("/") + "/completion", body)
                last_complete = clock.now()
                latency = 1000 * (last_complete - scheduled) / clock.frequency
                deadline = 60000 / args.apm
                deadline_misses += latency > deadline
                completed += 1
                diffusion = response.get("diffusion", {})
                timings = response["timings"]
                if response.get("timing_source", timing_source) != timing_source:
                    raise ValueError("Server timing semantics changed within a trial")
                row = {"action": completed, "start_utc": (utc + timedelta(seconds=(scheduled-origin)/clock.frequency)).isoformat(),
                       "start_elapsed_ms": 1000 * (scheduled-origin) / clock.frequency,
                       "latency_ms": latency, "deadline_ms": deadline, "deadline_missed": latency > deadline,
                       "service_ms": 1000 * (last_complete-dispatch) / clock.frequency,
                       "arrival_lateness_ms": 1000 * (dispatch-scheduled) / clock.frequency,
                       "prompt_tokens": timings["prompt_n"], "prompt_ms": timings["prompt_ms"],
                       "generated_tokens": timings["predicted_n"], "decode_ms": timings["predicted_ms"],
                       "decode_tokens_per_second": timings["predicted_per_second"],
                       "diffusion_steps": diffusion.get("steps_requested"),
                       "executed_forwards": diffusion.get("executed_forwards"),
                       "admission_wait_ms": diffusion.get("admission_wait_ms"),
                       "scheduled_qpc_ticks": str(scheduled), "dispatch_qpc_ticks": str(dispatch),
                       "completion_qpc_ticks": str(last_complete)}
                writer.writerow(row)
                csv_file.flush()
                responses.write(json.dumps({"arrival": index, "response": response}) + "\n")
                responses.flush()
                arrivals.write(json.dumps({"arrival": index, "outcome": "completed", "deadline_missed": latency > deadline}) + "\n")
                index += 1
                while index < offered and origin + arrival_ticks(index, args.apm, clock.frequency) < clock.now():
                    arrivals.write(json.dumps({"arrival": index, "outcome": "skipped-busy"}) + "\n")
                    index += 1
                    missed += 1
                arrivals.flush()
                status.update(actions=completed, skipped_arrivals=missed)
                atomic_json(args.output_dir / "status.json", status)
            if completed + missed != offered:
                raise AssertionError("Offered-arrival accounting failed")
        remaining = (end - clock.now()) / clock.frequency
        if remaining > 0:
            time.sleep(remaining)
        measured_end = max(end, last_complete)
        window.update(complete=True, end_qpc_ticks=str(measured_end))
        atomic_json(args.output_dir / "measurement.json", window)
        elapsed = (measured_end - origin) / clock.frequency
        summary = {"success": True, "actions": completed, "elapsed_seconds": elapsed,
                   "achieved_apm": 60 * completed / elapsed, "offered_arrivals": offered,
                   "skipped_arrivals": missed, "deadline_misses": deadline_misses,
                   "on_time_offered_fraction": (completed - deadline_misses) / offered,
                   "quality_evaluation": "not established by HTTP completion or action parsing",
                   "arrival_window_seconds": args.seconds, "drain_seconds": max(0, elapsed-args.seconds)}
        atomic_json(args.output_dir / "summary.json", summary)
        status.update(summary)
        print(json.dumps(summary), flush=True)
    except BaseException as exc:
        status.update(success=False, error=f"{type(exc).__name__}: {exc}")
        atomic_json(args.output_dir / "summary.json", status)
        raise
    finally:
        atomic_json(args.output_dir / "status.json", status)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", default="http://127.0.0.1:8080")
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--apm", type=int, default=100)
    parser.add_argument("--tokens", type=int, default=16)
    parser.add_argument("--steps", type=int)
    parser.add_argument("--prompt", default=VALIDATION_PROMPT)
    parser.add_argument("--warmup", type=int, default=1)
    parser.add_argument("--window-file", type=Path)
    args = parser.parse_args()
    if not 1 <= args.seconds <= 86400 or not 1 <= args.apm <= 6000 or not 1 <= args.tokens <= 2048 or args.warmup < 0:
        parser.error("Invalid duration, demand, token budget, or warmup")
    measure(args)


if __name__ == "__main__":
    main()
