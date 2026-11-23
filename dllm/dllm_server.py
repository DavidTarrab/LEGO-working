#!/usr/bin/env python3
"""Serial model owner with responsive health and independent cooperative controls."""
from __future__ import annotations

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import socket

from runtime import (GenerationCancelled, MODEL_ID, MODEL_REVISION, VALIDATION_PROMPT,
                     load_runtime, transfer_schedule)


def validate_request(request: dict, default_steps: int, default_skip: int) -> dict:
    if not isinstance(request, dict):
        raise ValueError("Request must be a JSON object")
    allowed = {"prompt", "n_predict", "steps", "skip_layers", "temperature", "seed",
               "cache_prompt", "stream"}
    unknown = set(request) - allowed
    if unknown:
        raise ValueError("Unsupported fields: " + ", ".join(sorted(unknown)))
    result = {"prompt": request.get("prompt", VALIDATION_PROMPT),
              "tokens": request.get("n_predict", 16),
              "steps": request.get("steps", default_steps),
              "skip_layers": request.get("skip_layers", default_skip),
              "temperature": request.get("temperature", 0),
              "seed": request.get("seed", 1)}
    transfer_schedule(result["tokens"], result["steps"])
    if not isinstance(result["prompt"], str) or not result["prompt"].strip():
        raise ValueError("prompt must be nonempty text")
    if type(result["skip_layers"]) is not int or not 0 <= result["skip_layers"] <= 14:
        raise ValueError("skip_layers must be an integer in 0..14")
    if type(result["temperature"]) not in (int, float) or result["temperature"] != 0 or type(result["seed"]) is not int:
        raise ValueError("Only deterministic temperature=0 with an integer seed is validated")
    if request.get("stream", False) is not False or request.get("cache_prompt", False) is not False:
        raise ValueError("Streaming and prompt caching are unsupported")
    return result


def completion_response(result, metadata: dict, skip_layers: int) -> dict:
    # Compatibility fields attribute tokenization/generation once, rather than
    # pretending the first denoising step is autoregressive prefill.
    return {"content": result.content, "model": metadata["model_id"],
            "model_metadata": metadata, "diffusion": result.to_dict(), "skip_layers": skip_layers,
            "timing_source": "diffusion-host-wall-v1",
            "timings": {"prompt_n": result.prompt_tokens,
                        "prompt_ms": result.tokenization_ms,
                        "predicted_n": len(result.output_ids),
                        "predicted_ms": result.generation_wall_ms,
                        "predicted_per_second": (1000 * len(result.output_ids) /
                                                 result.generation_wall_ms
                                                 if result.generation_wall_ms else 0)}}


class LegoServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, runtime, *, steps=16, skip_layers=0):
        self.runtime, self.steps, self.skip_layers = runtime, steps, skip_layers
        super().__init__(address, LegoCompatibleHandler)


class LegoCompatibleHandler(BaseHTTPRequestHandler):
    def setup(self):
        super().setup()
        self.connection.settimeout(15)

    def send_json(self, status: int, value: dict):
        body = json.dumps(value, allow_nan=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        try:
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError):
            pass

    def do_GET(self):
        if self.path.rstrip("/") in ("/health", "/status"):
            self.send_json(200, {"status": "ok", "model": self.server.runtime.metadata,
                                 "timing_source": "diffusion-host-wall-v1",
                                 "default_steps": self.server.steps,
                                 "default_skip_layers": self.server.skip_layers,
                                 "control": self.server.runtime.gate.status()})
        else:
            self.send_json(404, {"error": "Unknown endpoint"})

    def do_POST(self):
        try:
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 1024 * 1024 or self.headers.get("Transfer-Encoding"):
                raise ValueError("Require a body of 1..1048576 bytes with Content-Length")
            request = json.loads(self.rfile.read(length).decode("utf-8"))
            if self.path.rstrip("/") == "/control":
                if not isinstance(request, dict) or set(request) - {"action", "wait_ms"}:
                    raise ValueError("Control fields are action and optional wait_ms")
                action = request.get("action")
                wait_ms = request.get("wait_ms", 0)
                if type(wait_ms) is not int or not 0 <= wait_ms <= 5000:
                    raise ValueError("wait_ms must be an integer in 0..5000")
                gate = self.server.runtime.gate
                if action == "pause":
                    gate.pause()
                    if wait_ms:
                        gate.wait_paused(wait_ms / 1000)
                elif action == "resume":
                    gate.resume()
                elif action == "stop":
                    gate.stop()
                else:
                    raise ValueError("action must be pause, resume, or stop")
                self.send_json(200, gate.status())
            elif self.path.rstrip("/") == "/completion":
                config = validate_request(request, self.server.steps, self.server.skip_layers)
                result = self.server.runtime.generate(**config)
                self.send_json(200, completion_response(result, self.server.runtime.metadata,
                                                       config["skip_layers"]))
            else:
                self.send_json(404, {"error": "Unknown endpoint"})
        except (ValueError, UnicodeError, socket.timeout) as exc:
            self.send_json(400, {"error": str(exc)})
        except GenerationCancelled as exc:
            self.send_json(409, {"error": str(exc), "cancelled": True})
        except RuntimeError as exc:
            self.send_json(409 if "one model request" in str(exc) else 500,
                           {"error": str(exc)})
        except Exception as exc:
            self.send_json(500, {"error": f"{type(exc).__name__}: {exc}"})

    def log_message(self, *_):
        pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default=MODEL_ID)
    parser.add_argument("--revision", default=MODEL_REVISION)
    parser.add_argument("--device", default="cuda:0")
    parser.add_argument("--no-4bit", action="store_true")
    parser.add_argument("--cache-dir")
    parser.add_argument("--local-files-only", action="store_true")
    parser.add_argument("--steps", type=int, default=16)
    parser.add_argument("--skip-layers", type=int, default=0)
    parser.add_argument("--host", choices=["127.0.0.1"], default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8080)
    args = parser.parse_args()
    validate_request({"steps": args.steps, "skip_layers": args.skip_layers}, args.steps, args.skip_layers)
    runtime = load_runtime(model_id=args.model, revision=args.revision, device=args.device,
                           four_bit=not args.no_4bit, cache_dir=args.cache_dir,
                           local_files_only=args.local_files_only)
    server = LegoServer((args.host, args.port), runtime, steps=args.steps, skip_layers=args.skip_layers)
    print(json.dumps({"ready": True, "port": server.server_port, "metadata": runtime.metadata}), flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        runtime.gate.stop()
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
