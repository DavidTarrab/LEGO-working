[CmdletBinding()]
param(
    [string]$Archive,
    [string]$Destination
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$Archive) { $Archive = Join-Path $legoStorage 'downloads\cuda-12.8\cuda_12.8.1_572.61_windows.exe' }
if (!$Destination) { $Destination = $env:LEGO_CUDA_ROOT }
$Archive = [IO.Path]::GetFullPath($Archive)
$Destination = [IO.Path]::GetFullPath($Destination)
$extractionRoot = Join-Path $legoStorage 'temp\cuda-extract'
if (!(Test-Path -LiteralPath $Archive)) { throw "CUDA installer not found: $Archive" }
$null = New-Item -ItemType Directory -Force $extractionRoot, $Destination
# NVIDIA's installer is a self-extracting archive. Only unpack compiler/runtime
# components; do not install its bundled graphics driver or Visual Studio tools.
& tar.exe -xf $Archive -C $extractionRoot cuda_nvcc cuda_cudart cuda_cccl cuda_nvml_dev libcublas
if ($LASTEXITCODE -ne 0) { throw 'CUDA component extraction failed.' }
$components = @('cuda_nvcc\nvcc', 'cuda_cudart\cudart', 'cuda_cccl\thrust',
    'cuda_nvml_dev\nvml_dev', 'libcublas\cublas', 'libcublas\cublas_dev')
foreach ($component in $components) {
    $componentRoot = Join-Path $extractionRoot $component
    foreach ($directory in @('bin', 'include', 'lib', 'nvvm')) {
        $source = Join-Path $componentRoot $directory
        if (Test-Path -LiteralPath $source) {
            Copy-Item -LiteralPath $source -Destination $Destination -Recurse -Force
        }
    }
}
foreach ($required in @('bin\nvcc.exe', 'include\cuda_runtime.h', 'lib\x64\cudart.lib', 'lib\x64\cublas.lib')) {
    if (!(Test-Path -LiteralPath (Join-Path $Destination $required))) { throw "Missing CUDA component: $required" }
}
& (Join-Path $Destination 'bin\nvcc.exe') --version
if ($LASTEXITCODE -ne 0) { throw 'CUDA compiler did not start.' }
Write-Host "Portable CUDA toolkit ready: $Destination"
