[CmdletBinding()]
param([ValidateRange(1,16)][int]$Jobs=4)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
$engine = $env:LEGO_ENGINE_ROOT
$project = Join-Path $legoStorage 'DialogueUpstream\DialogueDemo.uproject'
$archive = Join-Path $legoStorage 'packaged-lego'
& (Join-Path $engine 'Engine\Build\BatchFiles\RunUAT.bat') BuildCookRun ('-project=' + $project) -noP4 -platform=Win64 -clientconfig=Development -build -cook '-map=/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap' -stage -pak -archive ('-archivedirectory=' + $archive) -unattended -utf8output -nocompileeditor -noxge ("-ubtargs=-MaxParallelActions=$Jobs -2019")
if ($LASTEXITCODE) { throw 'Build/cook/package failed. Inspect local\integration-notes\logs.' }
Write-Host "Packaged game: $archive\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe"
