[CmdletBinding()]
param(
    [ValidateSet('UE4Editor', 'DialogueDemoEditor', 'DialogueDemo', 'ShaderCompileWorker')][string]$Target = 'UE4Editor',
    [string]$Project,
    [ValidateRange(1, 16)][int]$Jobs = 4,
    [string]$LogName = 'engine-build.log'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
$engine = $env:LEGO_ENGINE_ROOT
$arguments = @($Target, 'Win64', 'Development', '-2019', '-NoXGE', "-MaxParallelActions=$Jobs", ('-Log=' + (Join-Path $legoDirectories.Logs $LogName)))
if ($Project) { $arguments += '-Project=' + [IO.Path]::GetFullPath($Project) }
& (Join-Path $engine 'Engine\Build\BatchFiles\Build.bat') @arguments
if ($LASTEXITCODE) { throw "Unreal build failed. Inspect $LogName." }
