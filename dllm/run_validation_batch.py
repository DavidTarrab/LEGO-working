"""Unattended correctness gate followed by repeated profiling; preserves failures."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

from profile_dllm import atomic_json


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache-dir", required=True)
    parser.add_argument("--reference-readme", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=False)
    source = Path(__file__).resolve().parent
    status = {"success": None, "completed": []}
    try:
        tasks = [
            ("correctness-4", ["validate_cuda.py", "--cache-dir", args.cache_dir,
                               "--reference-readme", args.reference_readme, "--tokens", "16", "--steps", "4"]),
            ("correctness-8", ["validate_cuda.py", "--cache-dir", args.cache_dir,
                               "--reference-readme", args.reference_readme, "--tokens", "16", "--steps", "8", "--skip-pause-tests"]),
            ("correctness-16", ["validate_cuda.py", "--cache-dir", args.cache_dir,
                                "--reference-readme", args.reference_readme, "--tokens", "16", "--steps", "16", "--skip-pause-tests"]),
            ("profile", ["profile_dllm.py", "--cache-dir", args.cache_dir,
                         "--local-files-only", "--steps", "4", "8", "16", "--repetitions", "3"])]
        for name, options in tasks:
            status["phase"] = name
            atomic_json(args.output_dir / "status.json", status)
            command = [sys.executable, "-u", str(source / options[0]), *options[1:],
                       "--output-dir", str(args.output_dir / name)]
            with (args.output_dir / f"{name}.log").open("x", encoding="utf-8") as log:
                result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(f"{name} exited with {result.returncode}; subsequent tasks refused")
            outcome = json.loads((args.output_dir / name / "status.json").read_text())
            if outcome.get("success") is not True:
                raise RuntimeError(f"{name} did not declare successful completion")
            status["completed"].append(name)
        status.update(success=True, phase="complete")
    except BaseException as exc:
        status.update(success=False, error=f"{type(exc).__name__}: {exc}")
        raise
    finally:
        atomic_json(args.output_dir / "status.json", status)


if __name__ == "__main__":
    main()
