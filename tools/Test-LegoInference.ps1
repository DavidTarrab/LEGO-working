[CmdletBinding()]
param(
    [string]$Model,
    [string]$OutputDirectory,
    [ValidateRange(1, 1024)][int]$CheckpointNodes = 1
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$Model) { $Model = $env:LEGO_MODEL_PATH }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $legoRoot ('runs\checkpoint-tests\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')) }
$runner = Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'lego-infer.exe'
if (!(Test-Path -LiteralPath $runner)) { throw 'Build first with tools\Build-LegoInference.ps1.' }
$logBase = Join-Path $legoDirectories.Logs ('checkpoint-' + [Guid]::NewGuid().ToString('N'))
$process = $null
try {
    # Redirect at process creation rather than through PowerShell's native
    # stderr pipeline, which can classify successful upstream logs as errors.
    $process = Start-Process -FilePath $runner -WindowStyle Hidden -PassThru -RedirectStandardOutput ($logBase + '.stdout.log') -RedirectStandardError ($logBase + '.stderr.log') -ArgumentList @(
        '--model', ('"' + $Model + '"'), '--output', ('"' + $OutputDirectory + '"'),
        '--tokens', '16', '--nodes', $CheckpointNodes, '--verify')
    $processHandle = $process.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    while (!$process.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Checkpoint verification timed out.' }
    }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Inference verification failed; inspect $logBase.stderr.log and $OutputDirectory." }
} finally {
    if ($process -and !$process.HasExited) { $process.Kill() }
    if ($process) { $process.Dispose() }
}
$result = Get-Content -LiteralPath (Join-Path $OutputDirectory 'summary.json') -Raw | ConvertFrom-Json
if (!$result.verification_passed) { throw 'Pause/resume verification did not pass.' }
Write-Host "Verified uninterrupted and paused output tokens match. Results: $OutputDirectory"
Write-Host "Upstream log: $logBase.stderr.log"
