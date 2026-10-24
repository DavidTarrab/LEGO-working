# Original rendering collector and data

`RenderHook/` contains the original Windows injection/collector source and
rendering CSVs. Compiled binaries, intermediate outputs and IDE state are
excluded. MinHook/HDE retain their source notices.

This collector is not part of the validated integrated trial runner. The current
runner measures the custom Unreal scene with PresentMon and shared game QPC
windows. The original collector has not been upgraded into a validated
commercial-game GPU capture/replay system. Its CSVs do not supply replayable GPU
command buffers and do not establish that another game's FPS can be measured
under a different scheduling policy.

Refer to the root README for supported experiments. Further collector/replay
work needs its own provenance, clock validation, replay fidelity and evaluation.
