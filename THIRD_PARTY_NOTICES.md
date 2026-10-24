# Third-party components

The root Apache 2.0 license applies to project code; it does not replace the
licenses of vendored dependencies or external assets.

| Component | Location / source | Notice |
| --- | --- | --- |
| LLaMA-Factory | `adaptor/LLaMA-Factory/` | Retain its `LICENSE`, attribution and upstream documentation. This tree includes research modifications. |
| lm-evaluation-harness | `adaptor/lm-evaluation-harness/` | Retain its `LICENSE.md` and task-specific dataset notices. This is a vendored research dependency. |
| LLM Colosseum | `fighting/colosseum/` | Retain `LICENCE` and upstream attribution. Game engine, ROM and service dependencies are obtained separately. |
| MinHook / HDE | `games/RenderHook/RenderHookDLL/Minhook/` | Copyright, redistribution conditions and disclaimers remain in the source headers. |
| llama.cpp / ggml | Fetched into ignored `local/llama-upstream/` | Pinned upstream license is copied into the native deployment bundle as `llama-LICENSE.txt`. |
| Unreal Engine | Separately authorized source checkout | Not distributed here. The staging tool applies project hooks to a user-supplied checkout. Obtain engine/template assets from Epic under its terms. |
| CUDA | User-supplied toolkit | Not distributed in Git. Local packaging copies required runtime DLLs; review NVIDIA's applicable redistribution terms before distributing a binary package. |
| PresentMon | User-supplied 1.5.2 executable | Obtain from its official project or authorized engine installation and retain its license when distributing it. |
| Models and datasets | User-supplied | Each publisher's terms apply. No model weights or trained adaptors are bundled with the validated runtime. |
| Paper | DOI linked in README | Publisher PDF is not distributed here. |

Original rendering CSVs under `games/` are research data, not captured GPU command
buffers or a complete commercial-game replay implementation. Their presence
does not imply that the Windows prototype reproduced those game evaluations.
