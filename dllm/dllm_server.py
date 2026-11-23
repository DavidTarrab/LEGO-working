#!/usr/bin/env python3
"""
Drop-in Diffusion LLM HTTP Server for LEGO Co-Location Experiments (Deliverable C).

Implements the exact `/health` and `/completion` JSON contract expected by
David's `tools/Measure-LlmLoad.ps1` and `tools/Analyze-ConcurrentRun.mjs`
(see `docs/EXTERNAL_BASELINE.md`).

Usage on the RTX 5070 Ti (or GCP GPU instance):
  Terminal A (start dLLM server):
    python dllm/dllm_server.py --model inclusionAI/LLaDA-MoE-7B-A1B-Instruct --steps 16 --port 8080

  Terminal B (run David's load generator + PresentMon):
    .\tools\Measure-LlmLoad.ps1 -Seconds 120 -APM 200 -OutputDir .\runs\dllm-concurrent-01\llm

  Analyze with David's offline HTML/SVG report generator:
    node .\tools\Analyze-ConcurrentRun.mjs .\runs\dllm-concurrent-01 --llm-baseline .\runs\dllm-only --fps 60
"""

import argparse
import json
from http.server import BaseHTTPRequestHandler, HTTPServer
import time
from typing import Any, Dict

from profile_dllm import (
    LEGO_VALIDATION_PROMPT,
    bottom_up_layer_skip,
    get_device,
    load_dllm,
    run_diffusion_steps,
)


class DllmServerState:
    def __init__(
        self,
        model_id: str,
        device: str,
        use_4bit: bool,
        default_steps: int,
        skip_layers: int,
    ):
        self.model_id = model_id
        self.device = device
        self.default_steps = default_steps
        self.skip_layers = skip_layers
        self.model, self.tokenizer, self.weights_vram_gb, self.num_layers = load_dllm(
            model_id, device=device, use_4bit=use_4bit
        )


STATE: DllmServerState = None  # type: ignore


class LegoCompatibleHandler(BaseHTTPRequestHandler):
    def _send_json(self, status_code: int, payload: Dict[str, Any]) -> None:
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status_code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path.rstrip("/") == "/health":
            self._send_json(
                200,
                {
                    "status": "ok",
                    "model": STATE.model_id,
                    "steps": STATE.default_steps,
                    "skip_layers": STATE.skip_layers,
                    "weights_vram_gb": round(STATE.weights_vram_gb, 2),
                },
            )
        else:
            self._send_json(404, {"error": "Not found"})

    def do_POST(self) -> None:
        if self.path.rstrip("/") != "/completion":
            self._send_json(404, {"error": "Not found"})
            return

        content_len = int(self.headers.get("Content-Length", "0"))
        raw_body = self.rfile.read(content_len).decode("utf-8") if content_len > 0 else "{}"
        req = json.loads(raw_body)

        prompt = req.get("prompt", LEGO_VALIDATION_PROMPT)
        n_predict = int(req.get("n_predict", 16))
        steps = int(req.get("steps", STATE.default_steps))
        skip_layers = int(req.get("skip_layers", STATE.skip_layers))

        prompt_tokens = len(STATE.tokenizer.encode(prompt, add_special_tokens=True))

        with bottom_up_layer_skip(STATE.model, skip_count=skip_layers, keep_last_layer=True):
            total_ms, step_ms_list, output_text = run_diffusion_steps(
                STATE.model,
                STATE.tokenizer,
                prompt=prompt,
                steps=steps,
                max_new_tokens=n_predict,
                device=STATE.device,
            )

        # Map Step 1 (prefill + initial denoising pass D_1) to `prompt_ms` and
        # remaining refinement steps (D_2 ... D_N) to `predicted_ms` so that
        # David's `tools/Measure-LlmLoad.ps1` and `tools/Analyze-ConcurrentRun.mjs`
        # work seamlessly without modifying a single line of PowerShell or JS.
        prompt_ms = step_ms_list[0] if step_ms_list else 0.0
        predicted_ms = sum(step_ms_list[1:]) if len(step_ms_list) > 1 else total_ms
        predicted_per_second = (1000.0 * n_predict / predicted_ms) if predicted_ms > 0 else 0.0

        response = {
            "content": output_text,
            "model": STATE.model_id,
            "diffusion_steps": steps,
            "skip_layers": skip_layers,
            "timings": {
                "prompt_n": prompt_tokens,
                "prompt_ms": round(prompt_ms, 4),
                "predicted_n": n_predict,
                "predicted_ms": round(predicted_ms, 4),
                "predicted_per_second": round(predicted_per_second, 4),
                "per_step_mean_ms": round(total_ms / max(1, steps), 4),
            },
        }
        self._send_json(200, response)

    def log_message(self, fmt: str, *args: Any) -> None:
        # Keep stdout clean during high-APM load tests
        return


def main() -> None:
    global STATE
    parser = argparse.ArgumentParser(description="LEGO-compatible Diffusion LLM HTTP Server")
    parser.add_argument(
        "--model",
        type=str,
        default="inclusionAI/LLaDA-MoE-7B-A1B-Instruct",
        help="HuggingFace dLLM identifier",
    )
    parser.add_argument("--host", type=str, default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--steps", type=int, default=16, help="Number of diffusion steps D_1..D_N")
    parser.add_argument("--skip-layers", type=int, default=0, help="Bottom-up skipped layers")
    parser.add_argument("--device", type=str, default=None)
    parser.add_argument("--no-4bit", action="store_true")
    args = parser.parse_args()

    device = get_device(args.device)
    STATE = DllmServerState(
        model_id=args.model,
        device=device,
        use_4bit=not args.no_4bit,
        default_steps=args.steps,
        skip_layers=args.skip_layers,
    )

    server = HTTPServer((args.host, args.port), LegoCompatibleHandler)
    print(
        f"[dLLM Server] Listening on http://{args.host}:{args.port} | "
        f"Model={args.model} | Steps={args.steps} | SkipLayers={args.skip_layers}"
    )
    print("[dLLM Server] Ready for .\\tools\\Measure-LlmLoad.ps1 !")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\n[dLLM Server] Shutting down.")
        server.server_close()


if __name__ == "__main__":
    main()
