import json
import threading
import time
import tempfile
from pathlib import Path
import unittest
from urllib.error import HTTPError
from urllib.request import Request, urlopen
from types import SimpleNamespace

from runtime import (CooperativeGate, GenerationCancelled, GenerationResult,
                     LladaRuntime, MASK_ID, parse_action, transfer_schedule)
from dllm_server import LegoServer, completion_response, validate_request
from measure_http import arrival_ticks, measure


class ProtocolTests(unittest.TestCase):
    def test_integer_qpc_arrivals_and_paced_http_ledger(self):
        self.assertEqual(arrival_ticks(1, 200, 10_000_000), 3_000_000)
        self.assertEqual(arrival_ticks(3, 200, 10_000_000), 9_000_000)
        gate = CooperativeGate()
        def generate(**_):
            time.sleep(.15)
            return GenerationResult("dodge", [7], [7], 2, 1, 1, [150], [1], 150, 0, 0, 0, "length")
        server = LegoServer(("127.0.0.1", 0), SimpleNamespace(
            metadata={"model_id": "stub-not-performance-evidence"}, gate=gate, generate=generate), steps=1)
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        try:
            with tempfile.TemporaryDirectory() as temporary:
                output = Path(temporary) / "trial"
                measure(SimpleNamespace(output_dir=output, server=f"http://127.0.0.1:{server.server_port}",
                                        prompt="test", tokens=1, steps=1, warmup=0, window_file=None,
                                        seconds=1, apm=600))
                summary = json.loads((output / "summary.json").read_text())
                ledger = [json.loads(line) for line in (output / "arrivals.jsonl").read_text().splitlines()]
                self.assertTrue(summary["success"])
                self.assertEqual(summary["offered_arrivals"], 10)
                self.assertEqual(summary["actions"] + summary["skipped_arrivals"], 10)
                self.assertEqual([row["arrival"] for row in ledger], list(range(10)))
                self.assertEqual(summary["on_time_offered_fraction"], 0)
                self.assertEqual(json.loads((output / "config.json").read_text())["timing_source"], "diffusion-host-wall-v1")
        finally:
            server.shutdown()
            server.server_close()
            serving.join(3)

    def test_schedule_conserves_tokens_and_distributes_remainder(self):
        self.assertEqual(transfer_schedule(10, 4), [3, 3, 2, 2])
        for tokens in range(1, 33):
            for steps in range(1, tokens + 1):
                schedule = transfer_schedule(tokens, steps)
                self.assertEqual(sum(schedule), tokens)
                self.assertEqual(len(schedule), steps)
                self.assertGreaterEqual(min(schedule), 1)
                self.assertLessEqual(max(schedule) - min(schedule), 1)

    def test_redundant_steps_and_invalid_requests_are_rejected(self):
        for tokens, steps in ((16, 32), (0, 1), (16, 0), (True, 1), (8, 1.0)):
            with self.assertRaises(ValueError):
                transfer_schedule(tokens, steps)
        for request in ([], {"steps": -1}, {"stream": True}, {"temperature": .2},
                        {"skip_layers": 99}, {"seed": True}, {"unknown": 4}, {"prompt": ""}, {"temperature": False}):
            with self.assertRaises(ValueError):
                validate_request(request, 4, 0)

    def test_output_count_and_one_step_timing_are_truthful(self):
        result = GenerationResult("dodge", [42], [42, 3, 3, 3], 10, 4, 1,
                                  [100], [4], 105, 2, 0, 0, "eos")
        response = completion_response(result, {"model_id": "test"}, 0)
        self.assertEqual(response["timings"]["predicted_n"], 1)
        self.assertEqual(response["timings"]["prompt_ms"], 2)
        self.assertEqual(response["timings"]["predicted_ms"], 105)
        self.assertEqual(response["diffusion"]["requested_tokens"], 4)
        self.assertEqual(response["diffusion"]["executed_forwards"], 1)
        self.assertEqual(response["timing_source"], "diffusion-host-wall-v1")

    def test_action_parser_does_not_accept_substrings_or_explanations_first(self):
        self.assertEqual(parse_action(" Dodge. Avoid the sword."), "dodge")
        self.assertIsNone(parse_action("attacker"))
        self.assertIsNone(parse_action("I would attack"))

    def test_pause_acknowledgement_and_stop_release(self):
        gate = CooperativeGate()
        gate.begin()
        gate.pause()
        self.assertFalse(gate.status()["paused"])
        stopped = threading.Event()
        def worker():
            try:
                gate.checkpoint()
                self.fail("Stopped gate returned normally")
            except GenerationCancelled:
                stopped.set()
            finally:
                gate.finish()
        thread = threading.Thread(target=worker)
        thread.start()
        self.assertTrue(gate.wait_paused(2))
        self.assertEqual(gate.status()["completed_chunks"], 0)
        gate.stop()
        thread.join(2)
        self.assertFalse(thread.is_alive())
        self.assertTrue(stopped.is_set())

    def test_resume_without_lost_wakeup(self):
        for _ in range(30):
            gate = CooperativeGate()
            gate.begin()
            gate.pause()
            done = threading.Event()
            thread = threading.Thread(target=lambda: (gate.checkpoint(), done.set()))
            thread.start()
            gate.resume()
            thread.join(2)
            self.assertTrue(done.is_set())
            gate.finish()

    def test_health_and_resume_remain_responsive_while_completion_paused(self):
        gate = CooperativeGate()
        def generate(**kwargs):
            gate.begin()
            try:
                gate.checkpoint()
                return GenerationResult("dodge", [7], [7], 2, 1, 1, [1], [1], 2, 0, 0, 0, "length")
            finally:
                gate.finish()
        server = LegoServer(("127.0.0.1", 0), SimpleNamespace(
            metadata={"model_id": "stub-not-gpu-evidence"}, gate=gate, generate=generate), steps=1)
        serving = threading.Thread(target=server.serve_forever, daemon=True)
        serving.start()
        address = f"http://127.0.0.1:{server.server_port}"
        def request(path, value=None):
            body = None if value is None else json.dumps(value).encode()
            with urlopen(Request(address + path, data=body,
                                 headers={"Content-Type": "application/json"}), timeout=3) as response:
                return json.load(response)
        results = []
        gate.pause()
        completion = threading.Thread(target=lambda: results.append(request("/completion", {"n_predict": 1})))
        try:
            completion.start()
            deadline = time.monotonic() + 2
            with gate.condition:
                gate.condition.wait_for(lambda: gate.paused, max(0, deadline - time.monotonic()))
            self.assertTrue(gate.status()["paused"])
            self.assertEqual(request("/health")["status"], "ok")
            request("/control", {"action": "resume"})
            completion.join(3)
            self.assertFalse(completion.is_alive())
            self.assertEqual(results[0]["content"], "dodge")
            with self.assertRaises(HTTPError) as failure:
                request("/completion", {"steps": 32})
            self.assertEqual(failure.exception.code, 400)
        finally:
            gate.stop()
            completion.join(3)
            server.shutdown()
            server.server_close()
            serving.join(3)


try:
    import torch
except ImportError:
    torch = None


@unittest.skipIf(torch is None, "Install CPU or CUDA PyTorch for numerical sampler tests")
class NumericalTests(unittest.TestCase):
    def runtime(self):
        class Tokenizer:
            eos_token_id, unk_token_id = 3, None
            mask_token_id = MASK_ID
            all_special_ids = [3, 4, MASK_ID]
            def convert_ids_to_tokens(self, index):
                return "<|mask|>" if index == MASK_ID else str(index)
            def convert_tokens_to_ids(self, token):
                return {"<|mask|>": MASK_ID, "<|role_end|>": 4}.get(token)
            def apply_chat_template(self, *args, **kwargs):
                return "formatted"
            def __call__(self, *args, **kwargs):
                return SimpleNamespace(input_ids=torch.tensor([[1, 2]]))
            def decode(self, ids, **kwargs):
                return " ".join(str(i) for i in ids)
        class Model(torch.nn.Module):
            def __init__(self):
                super().__init__()
                self.config = SimpleNamespace(max_position_embeddings=32, num_hidden_layers=4)
                self.model = SimpleNamespace(layers=torch.nn.ModuleList([torch.nn.Identity() for _ in range(4)]))
            def forward(self, input_ids, **kwargs):
                logits = torch.full((*input_ids.shape, MASK_ID + 1), -10.)
                for pos in range(input_ids.shape[1]):
                    token = 7 + pos if pos < 4 else 3
                    logits[0, pos, token] = 10. + pos
                return SimpleNamespace(logits=logits)
        return LladaRuntime(Model(), Tokenizer(), "cpu", {"model_id": "synthetic-test"})

    def test_complete_canvas_prompt_preservation_and_early_eos_count(self):
        runtime = self.runtime()
        states = []
        result = runtime.generate("x", tokens=4, steps=3, on_step=lambda _, x: states.append(x))
        self.assertEqual(result.canvas_ids, [9, 10, 3, 3])
        self.assertEqual(result.output_ids, [9, 10])
        self.assertEqual(result.transfer_counts, [2, 1, 1])
        self.assertEqual([state.count(MASK_ID) for state in states], [2, 1, 0])
        self.assertGreaterEqual(result.generation_wall_ms, sum(result.step_wall_ms))

    def test_skipping_restores_modules_and_config_after_exception(self):
        runtime = self.runtime()
        original = runtime.model.model.layers
        with self.assertRaisesRegex(RuntimeError, "intentional"):
            with runtime.skip_layers(2) as skipped:
                self.assertEqual(skipped, [1, 2])
                self.assertEqual(len(runtime.model.model.layers), 2)
                raise RuntimeError("intentional")
        self.assertIs(runtime.model.model.layers, original)
        self.assertEqual(runtime.model.config.num_hidden_layers, 4)
        with self.assertRaises(ValueError):
            with runtime.skip_layers(3):
                pass

    def test_wrong_mask_is_rejected_before_generation(self):
        runtime = self.runtime()
        runtime.tokenizer.convert_ids_to_tokens = lambda _: "<eos>"
        with self.assertRaisesRegex(ValueError, "mask"):
            LladaRuntime(runtime.model, runtime.tokenizer, "cpu", {})


if __name__ == "__main__":
    unittest.main()
