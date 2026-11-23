# Dot-source before CUDA Python commands. No global settings are changed.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$env:LEGO_PYTHON) { $env:LEGO_PYTHON = Join-Path $legoStorage 'toolchains\python312\python.exe' }
if (!(Test-Path -LiteralPath $env:LEGO_PYTHON)) { throw 'Set LEGO_PYTHON to a Python 3.12 executable with dllm/requirements.txt installed.' }
$env:HF_HOME = Join-Path $legoStorage 'cache\huggingface'
$env:HF_MODULES_CACHE = Join-Path $env:HF_HOME 'modules'
$env:HF_HUB_DISABLE_TELEMETRY = '1'
$env:HF_HUB_DISABLE_XET = '1'
$env:PIP_CACHE_DIR = Join-Path $legoStorage 'cache\pip'
$env:MPLCONFIGDIR = Join-Path $legoStorage 'cache\matplotlib'
$env:PYTHONHASHSEED = '1'
Write-Host "CUDA Python: $env:LEGO_PYTHON"
