[CmdletBinding()]
param([ValidateRange(1, 16)][int]$Jobs = 2, [ValidatePattern('^[0-9]+$')][string]$CudaArchitecture = '120')
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
$source = Join-Path $legoStorage 'llama-upstream'
$expected = '0a1ef309cdbb9f2024f58a38fb4c42352c5db569'
if (!(Test-Path -LiteralPath $source)) {
    & git init $source
    if ($LASTEXITCODE) { throw 'Upstream repository initialization failed.' }
    & git -C $source remote add origin https://github.com/ggml-org/llama.cpp.git
    if ($LASTEXITCODE) { throw 'Cannot configure upstream remote.' }
    & git -C $source fetch --depth 1 origin $expected
    if ($LASTEXITCODE) { throw 'Upstream clone failed.' }
    & git -C $source checkout --detach $expected
    if ($LASTEXITCODE) { throw 'Cannot select pinned upstream commit.' }
}
$actual = & git -C $source rev-parse HEAD
if ($LASTEXITCODE -or $actual -ne $expected) { throw "Expected upstream $expected; found $actual. Existing checkout was not changed." }
$sourceChanges = & git -C $source status --porcelain
if ($LASTEXITCODE -or $sourceChanges) { throw 'Upstream source has changes; refusing to build an unrecorded variant.' }
$vs = $env:LEGO_VS_ROOT
if (!$vs) { throw 'Visual Studio 2019 C++ build tools not found. Set LEGO_VS_ROOT to the installation directory.' }
$cmake = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$ninja = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe'
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$build = Join-Path $legoStorage 'build\lego-inference'
foreach ($required in @($cmake, $ninja, $vcvars, (Join-Path $env:LEGO_CUDA_ROOT 'bin\nvcc.exe'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "Build prerequisite missing: $required" }
}
$env:CUDA_PATH = $env:LEGO_CUDA_ROOT
$env:PATH = (Join-Path $env:LEGO_CUDA_ROOT 'bin') + ';' + $env:PATH
$configure = '"{0}" -S "{1}" -B "{2}" -G Ninja -DCMAKE_MAKE_PROGRAM="{3}" -DCMAKE_BUILD_TYPE=Release -DCMAKE_CUDA_ARCHITECTURES={5} -DCUDAToolkit_ROOT="{4}" -DCMAKE_CUDA_COMPILER="{4}/bin/nvcc.exe"' -f $cmake, (Join-Path $PSScriptRoot 'native'), $build, $ninja, ($env:LEGO_CUDA_ROOT -replace '\\','/'), $CudaArchitecture
& cmd.exe /d /c ('""{0}" >nul && {1}"' -f $vcvars, $configure)
if ($LASTEXITCODE) { throw 'CMake configuration failed.' }
& cmd.exe /d /c ('""{0}" >nul && "{1}" --build "{2}" --parallel {3}"' -f $vcvars, $cmake, $build, $Jobs)
if ($LASTEXITCODE) { throw 'Native inference build failed.' }
foreach ($pattern in @('cudart64*.dll', 'cublas64*.dll', 'cublasLt64*.dll')) {
    Get-ChildItem -LiteralPath (Join-Path $env:LEGO_CUDA_ROOT 'bin') -Filter $pattern | Copy-Item -Destination (Join-Path $build 'bin') -Force
}
Copy-Item -LiteralPath (Join-Path $source 'LICENSE') -Destination (Join-Path $build 'bin\llama-LICENSE.txt') -Force
$null = New-Item -ItemType Directory -Force (Join-Path $build 'bin\include')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'native\LegoInference.h') -Destination (Join-Path $build 'bin\include\LegoInference.h') -Force
@{ upstream_commit = $expected; cuda_root = $env:LEGO_CUDA_ROOT; cuda_graphs = $false; requested_cuda_architecture = $CudaArchitecture; build_type = 'Release' } |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $build 'bin\build-info.json') -Encoding UTF8
Write-Host "LEGO inference runner: $build\bin\lego-infer.exe"
