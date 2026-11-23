# Diffusion LLM (dLLM) Evaluation & Co-Location Harness (`dllm/`)

This directory implements the **Weeks 1–4 Diffusion LLM (dLLM)** deliverables for the COMS E6998 LEGO project and plugs directly into the existing Windows measurement and analysis scripts (`tools/Measure-LlmLoad.ps1` and `tools/Analyze-ConcurrentRun.mjs`).

---

## 1. Quick Setup (GCP GPU Instance or Windows RTX 5070 Ti)

```bash
python -m venv .venv-dllm
# On Linux / macOS:
source .venv-dllm/bin/activate
# On Windows PowerShell:
# .\.venv-dllm\Scripts\Activate.ps1

pip install -r dllm/requirements.txt
```

---

## 2. Deliverable A & B: Standalone dLLM Profiling, Step Budgets & Layer Similarity Heatmap

Generate the **16 GB VRAM budget evaluation table** (comparing `google/diffusiongemma-26B-A4B-it`, `inclusionAI/LLaDA-MoE-7B-A1B-Instruct`, `Dream-org/Dream-v0-Instruct-7B`, and `GSAI-ML/LLaDA-8B-Instruct` against David's Llama-3.1-8B Q4_K_M baseline in `docs/results/validation-01.json`):

```bash
python dllm/profile_dllm.py --table-only --output-dir runs/dllm-profile
```

Run the full GPU profiling suite (per-step latency across $N \in \{4, 8, 16, 32, 64\}$ steps, bottom-up layer skipping $k \in \{0, 2, 4, 6, 8\}$, and LEGO Fig. 8 inter-layer cosine similarity heatmap):

```bash
# Option 1: LLaDA-MoE-7B-A1B (7B total, 1.4B active per step — fits in ~5.1 GB VRAM with 4-bit NF4)
python dllm/profile_dllm.py --model inclusionAI/LLaDA-MoE-7B-A1B-Instruct --output-dir runs/dllm-llada-moe

# Option 2: Dream-v0-Instruct-7B (Dense 7B discrete diffusion LLM)
python dllm/profile_dllm.py --model Dream-org/Dream-v0-Instruct-7B --output-dir runs/dllm-dream-7b
```

Outputs in `runs/dllm-*`:
- `dllm_week1_4_characterization.json` (VRAM, per-step latency, achievable APM, bottom-up layer skipping)
- `dllm_layer_similarity_heatmap.png` (LEGO Fig. 8 reproduction for dLLM)

---

## 3. Deliverable C: Naive Game + dLLM Co-Location via David's Harness

`dllm/dllm_server.py` implements the exact `GET /health` and `POST /completion` JSON schema used by `tools/Measure-LlmLoad.ps1` and `docs/EXTERNAL_BASELINE.md`.

### Step 1: Start the dLLM Server (Terminal A)
```powershell
python .\dllm\dllm_server.py --model inclusionAI/LLaDA-MoE-7B-A1B-Instruct --steps 16 --skip-layers 0 --port 8080
```

### Step 2: Measure dLLM-Only Baseline (Terminal B)
```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
Invoke-RestMethod http://127.0.0.1:8080/health
.\tools\Measure-LlmLoad.ps1 -Seconds 60 -APM 200 -OutputDir .\runs\dllm-only
```

### Step 3: Measure Naive Concurrent Game + dLLM Contention (Elevated Terminal B & C)
Follow `docs/EXTERNAL_BASELINE.md` while `dllm_server.py` is running on port 8080:
```powershell
# Elevated Terminal B (Launch Unreal Game + PresentMon)
$game = Join-Path $PWD 'local\packaged-lego\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe'
Start-Process -FilePath $game -WindowStyle Normal -ArgumentList @(
  '-dx12','-windowed','-ResX=1920','-ResY=1080','-ExecCmds=t.MaxFPS 60,r.VSync 0')
$trial = Join-Path $PWD 'runs\dllm-concurrent-01'
New-Item -ItemType Directory -Path $trial -Force
. .\tools\Enter-LegoEnvironment.ps1
& $env:LEGO_PRESENTMON -process_name DialogueDemo.exe `
  -output_file "$trial\frames.csv" -timed 60 -terminate_after_timed -dont_restart_as_admin
```

While PresentMon is capturing, run the dLLM load in Terminal C:
```powershell
.\tools\Measure-LlmLoad.ps1 -Seconds 60 -APM 200 -OutputDir .\runs\dllm-concurrent-01\llm
```

### Step 4: Generate Offline HTML / SVG Reports & Metrics
```powershell
node .\tools\Analyze-ConcurrentRun.mjs .\runs\dllm-concurrent-01 `
  --llm-baseline .\runs\dllm-only --gpu-utc-offset -04:00 --fps 60
```
Open `runs/dllm-concurrent-01/analysis/report.html` for full frame-time p99, GPU utilization, and dLLM latency charts!
