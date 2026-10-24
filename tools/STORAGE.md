# Local storage and configuration

Run scripts from the repository root. Dot-source the environment when launching
tools manually; build/test/capture scripts also load it themselves:

```powershell
Set-ExecutionPolicy -Scope Process -ExecutionPolicy Bypass
. .\tools\Enter-LegoEnvironment.ps1
```

Generated data stays on the repository's drive by default. Put the checkout on
a volume with enough capacity for a full source engine, toolchains and builds.
No global environment settings or system-wide execution policies are changed.

| Setting | Default / discovery |
| --- | --- |
| `TEMP`, `TMP` | `local/temp` |
| Unreal local DDC | `local/cache/DerivedDataCache` |
| CUDA cache | `local/cache/CUDA` |
| Automation/build logs | `local/integration-notes/logs` |
| `LEGO_ENGINE_ROOT` | `local/UE427Source` |
| `LEGO_MODEL_PATH` | `local/models/model.gguf` |
| `LEGO_UPSTREAM_BACKEND_DIR` | `local/build/lego-inference/bin` |
| `LEGO_VS_ROOT` | `local/toolchains/VS2019`, otherwise VS2019 discovery via `vswhere` |
| `LEGO_CUDA_ROOT` | `local/toolchains/cuda-12.8`, otherwise `CUDA_PATH` |
| `LEGO_PRESENTMON` | `local/tools/PresentMon64-1.5.2.exe` |

Set `LEGO_*` overrides **before** loading the environment. Existing overrides
are preserved. Use actual absolute installation paths for external dependencies.
The backend currently builds into `local/build/lego-inference`; if overriding
its deployment directory, copy the complete built bundle, including headers,
import library, build metadata and license, to that directory.

The packaged game and generated integration project remain under
`local/packaged-lego` and `local/DialogueUpstream`. Capture runners set the game
user directory under the trial to keep its saved files together. Windows-managed
shared SDK components can still use the system drive.

`local/` and `runs/` are ignored. They may contain credentials in third-party
tools, licensed dependencies, private paths and model weights; do not force-add
these directories. Publish only deliberately selected, sanitized result data.
