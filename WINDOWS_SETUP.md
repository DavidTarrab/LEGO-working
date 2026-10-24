# Windows setup and build

Commands assume PowerShell at the repository root. Dependencies, models and
build outputs go under ignored `local/`; results go under ignored `runs/`.
No particular drive or checkout name is required.

## 1. Prerequisites

Install Git, Node.js 18+, Visual Studio **2019** C++ build tools with CMake/Ninja,
Windows SDK **10.0.19041.0**, .NET Framework **4.6.2 Developer Pack**, and CUDA
Toolkit **12.8**. Use a driver compatible with the toolkit and GPU.
`nvidia-smi` reports driver compatibility, not the installed compiler; check
`nvcc --version` separately.

The scripts discover VS2019 with `vswhere` or `local/toolchains/VS2019`. CUDA is
found at `local/toolchains/cuda-12.8` or through `CUDA_PATH`. Set environment
variables before loading the environment to override locations:

```powershell
# $env:LEGO_VS_ROOT = '<Visual Studio 2019 installation>'
# $env:LEGO_CUDA_ROOT = '<CUDA 12.8 toolkit>'
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoEnvironment.ps1
```

Optional: `Prepare-PortableCuda.ps1 -Archive <downloaded-installer>` extracts the
compiler/runtime components to local storage without installing its bundled
driver. It was tested with NVIDIA's CUDA 12.8.1 Windows installer. It does not
replace the other prerequisites. See [storage configuration](tools/STORAGE.md).

## 2. Licensed source engine

Obtain authorized access to Epic's private GitHub repository by linking the
required Epic Games and GitHub accounts. Then:

```powershell
git clone --branch 4.27.2-release --single-branch `
  https://github.com/EpicGames/UnrealEngine.git .\local\UE427Source
git -C .\local\UE427Source checkout d94b38ae3446da52224bedd2568c078f828b4039
. .\tools\Enter-LegoEnvironment.ps1
& "$env:LEGO_ENGINE_ROOT\Setup.bat"
& "$env:LEGO_ENGINE_ROOT\GenerateProjectFiles.bat" -2019
.\tools\Build-LegoEngine.ps1 -Target UE4Editor
.\tools\Build-LegoEngine.ps1 -Target ShaderCompileWorker
```

If the old dependency manifest fails, this helper obtains the corrected manifest
from Epic's official 4.27.2 release using an existing Git credential:

```powershell
node .\tools\Fetch-EpicDependencyManifest.mjs .\local\UE427Source
```

Rerun `Setup.bat` afterwards. The helper never prints or stores the credential.
The licensed checkout must remain outside public commits. To use a checkout
elsewhere, set `LEGO_ENGINE_ROOT` before loading the environment and use that
same directory for staging.

## 3. Template content and model

Launch the built editor and create a **Third Person / Blueprint** project named
`LegoBaseline` at `local/LegoBaseline`, with desktop settings and no starter
content. Save the default map and close the editor. Confirm these exist:

```text
local/LegoBaseline/Content/ThirdPersonBP/Maps/ThirdPersonExampleMap.umap
local/LegoBaseline/Content/ThirdPersonBP/Blueprints/ThirdPersonCharacter.uasset
```

The integration copies this content to a separate C++ project. These Epic assets
are not distributed in the public repository.

Obtain a GGUF under its model license. The validated model was **Meta Llama 3.1
8B Instruct Q4_K_M**. Place it at `local/models/model.gguf` or set `LEGO_MODEL_PATH`
to its absolute filename. Record the source and checksum for future experiments;
a different model, quantization or prompt is a different workload. The original
validation did not record a weight checksum, so its exact weight identity cannot
be established from the portable result record alone.

## 4. Build and verify inference

```powershell
.\tools\Build-LegoInference.ps1 -CudaArchitecture 120
.\tools\Test-LegoInference.ps1
.\tools\Test-LegoInferenceControl.ps1
```

Architecture `120` is the validated RTX 5070 Ti target. Choose the correct CUDA
architecture for other GPUs. The script fetches pinned upstream llama.cpp,
checks its revision, builds the bridge and stages matching runtime DLLs in
`local/build/lego-inference/bin`. Upstream source remains unmodified.

The first test checks output equivalence and acknowledged pauses within prefill
and token decoding. The second checks external controls, including stop while
paused. New output goes under `runs/checkpoint-tests` and `runs/control-tests`.
See [backend semantics](tools/UPSTREAM_INFERENCE.md).

## 5. Integrate and package

```powershell
node .\tools\Prepare-LegoIntegration.mjs stage --apply
node .\tools\Prepare-LegoIntegration.mjs project
.\tools\Build-LegoEngine.ps1 -Target DialogueDemoEditor `
  -Project (Join-Path $PWD 'local\DialogueUpstream\DialogueDemo.uproject')
.\tools\Test-LegoGame.ps1 -Policy 2
.\tools\Test-LegoGame.ps1 -Policy 1
.\tools\Test-LegoGame.ps1 -Policy 0
.\tools\Build-LegoPackage.ps1
```

`stage` without `--apply` previews changes. It validates exact engine anchors and
stages only project-owned modules. `project` refuses an existing destination;
optional arguments are `project <destination> <baseline-directory> <model-file>`.
For later source updates use `sync-project local/DialogueUpstream` and rebuild.

Packaging compiles a standalone executable, cooks assets/shaders and stages
runtime DLLs. Shader compilation can use substantial CPU without a game window.
Wait for `BUILD SUCCESSFUL`. The experiment executable is the nested binary:

```text
local/packaged-lego/WindowsNoEditor/DialogueDemo/Binaries/Win64/DialogueDemo.exe
```

Keep adjacent DLLs together. The model stays external. Do not rename the new
bridge to the original missing backend DLL.

## 6. Capture and analyze

Obtain **PresentMon 1.5.2** from its official release or an authorized UE
installation. Set `LEGO_PRESENTMON` or copy it to
`local/tools/PresentMon64-1.5.2.exe`. The parser expects 1.x columns; newer releases
are not drop-in replacements.

Use **Administrator PowerShell** for rendered capture. Follow the single-trial
or matrix commands in [README.md](README.md). Start with the short validation,
inspect `status.json` and `summary.md`, then use longer repetitions. Integrated
runs do not require a separate inference server.

## Troubleshooting

| Symptom | Action |
| --- | --- |
| Scripts are disabled | Set execution policy to Bypass for this process; no system-wide policy change is needed |
| PresentMon error 5 / ETW session failure | Use an Administrator terminal |
| Backend library/DLL missing | Build inference first and keep staged dependencies together |
| Admission timeout / missing rendering hooks | Use the hooked source engine/package and D3D12; inspect `game.log` and hook counters |
| Project exists | Use `sync-project` or select a new destination |
| Independently edited source detected | Review the difference; synchronization refuses silent overwrites |
| Scheduled inference misses deadlines | Inspect waits, skipped arrivals and workload capacity; capture success does not mean both targets are achievable |

Windows-managed shared SDK components can use the system drive even though
project caches and build outputs stay local. See [runtime details](tools/STEP6.md).
