[CmdletBinding()]
param(
    [ValidateRange(1, 120)][int]$Seconds = 3,
    [ValidateRange(0, 2)][int]$Policy = 2,
    [switch]$ExpectHookFailure
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
$engine = $env:LEGO_ENGINE_ROOT
$projectRoot = Join-Path $legoStorage 'DialogueUpstream'
$game = Join-Path $engine 'Engine\Binaries\Win64\UE4Editor.exe'
$project = Join-Path $projectRoot 'DialogueDemo.uproject'
$testRoot = Join-Path $legoRoot ('runs\integration-smoke\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $testRoot
$arguments = @(
    ('"' + $project + '"'), '/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap?game=/Script/DialogueDemo.LegoExperimentGameMode',
    '-game', '-dx12', '-windowed', '-ResX=1280', '-ResY=720', '-unattended', '-nosplash', '-nosound',
    '-LegoAutoStart', '-LegoQuitAfterRun', "-LegoSeconds=$Seconds", '-LegoAPM=60', "-LegoPolicy=$Policy",
    ('-LegoModel="' + $env:LEGO_MODEL_PATH + '"'),
    ('-LegoOutput="' + (Join-Path $testRoot 'llm') + '"'),
    ('-abslog="' + (Join-Path $testRoot 'game.log') + '"'),
    ('-UserDir="' + (Join-Path $testRoot 'userdata') + '"'), '-stdout', '-FullStdOutLogOutput')
$process = $null
try {
    $process = Start-Process -FilePath $game -ArgumentList $arguments -WindowStyle Normal -PassThru -RedirectStandardOutput (Join-Path $testRoot 'stdout.log') -RedirectStandardError (Join-Path $testRoot 'stderr.log')
    $processHandle = $process.Handle
    $deadline = [DateTime]::UtcNow.AddMinutes(5)
    while (!$process.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw "Game smoke test timed out. Inspect $testRoot\game.log." }
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Game exit code '$($process.ExitCode)'. Inspect $testRoot\game.log." }
    $summaries = @(Get-ChildItem -LiteralPath (Join-Path $testRoot 'llm') -Filter summary.json -Recurse -File)
    if ($summaries.Count -ne 1) { throw "Expected one inference summary. Inspect $testRoot." }
    $summary = Get-Content -LiteralPath $summaries[0].FullName -Raw | ConvertFrom-Json
    if ($ExpectHookFailure) {
        if ($summary.success -or $summary.error -notmatch 'Rendering hooks') { throw 'Expected the missing-hook guard to reject scheduling.' }
        Write-Host "Missing-hook rejection verified: $testRoot"
    } else {
        if (!$summary.success -or $summary.actions -lt 1) { throw "Integrated inference did not complete: $($summary.error)" }
        if ($summary.observed_render_frames -lt 1 -or $summary.observed_gpu_submissions -lt 1) { throw 'The hooked source-engine run did not observe rendering activity.' }
        Write-Host "Integrated game test passed: actions=$($summary.actions), frames=$($summary.observed_render_frames), submissions=$($summary.observed_gpu_submissions), output=$testRoot"
    }
} finally {
    if ($process -and !$process.HasExited) { $process.Kill() }
    if ($process) { $process.Dispose() }
}
