# Dot-source before builds and experiments to keep generated data local.
# This affects this process and its children, not unrelated applications.
$legoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$legoStorage = Join-Path $legoRoot 'local'
$legoDirectories = @{
    Temp = Join-Path $legoStorage 'temp'
    Downloads = Join-Path $legoStorage 'downloads'
    DerivedData = Join-Path $legoStorage 'cache\DerivedDataCache'
    CudaCache = Join-Path $legoStorage 'cache\CUDA'
    Logs = Join-Path $legoStorage 'integration-notes\logs'
}
foreach ($legoDirectory in $legoDirectories.Values) {
    $null = New-Item -ItemType Directory -Force $legoDirectory
}
$env:TEMP = $legoDirectories.Temp
$env:TMP = $legoDirectories.Temp
$env:CUDA_CACHE_PATH = $legoDirectories.CudaCache
[Environment]::SetEnvironmentVariable('UE-LocalDataCachePath', $legoDirectories.DerivedData, 'Process')
[Environment]::SetEnvironmentVariable('UE-SharedDataCachePath', 'None', 'Process')
$env:uebp_LogFolder = $legoDirectories.Logs
if (!$env:LEGO_CUDA_ROOT) {
    $portableCuda = Join-Path $legoStorage 'toolchains\cuda-12.8'
    $env:LEGO_CUDA_ROOT = if (Test-Path -LiteralPath $portableCuda) { $portableCuda } elseif ($env:CUDA_PATH) { $env:CUDA_PATH } else { $portableCuda }
}
if (!$env:LEGO_VS_ROOT) {
    $portableVS = Join-Path $legoStorage 'toolchains\VS2019'
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $portableVS) { $env:LEGO_VS_ROOT = $portableVS }
    elseif (Test-Path -LiteralPath $vswhere) { $env:LEGO_VS_ROOT = & $vswhere -version '[16.0,17.0)' -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1 }
}
if (!$env:LEGO_ENGINE_ROOT) { $env:LEGO_ENGINE_ROOT = Join-Path $legoStorage 'UE427Source' }
if (!$env:LEGO_MODEL_PATH) { $env:LEGO_MODEL_PATH = Join-Path $legoStorage 'models\model.gguf' }
if (!$env:LEGO_UPSTREAM_BACKEND_DIR) { $env:LEGO_UPSTREAM_BACKEND_DIR = Join-Path $legoStorage 'build\lego-inference\bin' }
if (!$env:LEGO_PRESENTMON) { $env:LEGO_PRESENTMON = Join-Path $legoStorage 'tools\PresentMon64-1.5.2.exe' }
Write-Host "LEGO temporary files: $env:TEMP"
Write-Host "Unreal Derived Data Cache: $($legoDirectories.DerivedData)"
