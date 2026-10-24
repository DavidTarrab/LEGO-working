[CmdletBinding()]
param([string]$Model, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$Model) { $Model = $env:LEGO_MODEL_PATH }
if (!$OutputDirectory) { $OutputDirectory = Join-Path $legoRoot ('runs\control-tests\' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new test output directory.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$runner = Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'lego-infer.exe'
$name = 'test-' + [Guid]::NewGuid().ToString('N')
$stdout = Join-Path $OutputDirectory 'stdout.log'
$stderr = Join-Path $OutputDirectory 'stderr.log'
$inferenceOutput = Join-Path $OutputDirectory 'inference'
$process = $null
try {
    $process = Start-Process -FilePath $runner -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr -ArgumentList @(
        '--model', ('"' + $Model + '"'), '--output', ('"' + $inferenceOutput + '"'),
        '--tokens', '256', '--nodes', '1', '--control-name', $name)
    # Keep a process handle before it exits; Windows PowerShell otherwise can
    # lose the exit-code association for a rapidly terminating Start-Process.
    $processHandle = $process.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(120)
    do {
        if ($process.HasExited) { throw "Runner exited before control test. Inspect $stderr." }
        if ((Test-Path -LiteralPath $stdout) -and (Select-String -LiteralPath $stdout -Pattern '^READY' -Quiet)) { break }
        Start-Sleep -Milliseconds 50
    } while ([DateTime]::UtcNow -lt $deadline)
    if ([DateTime]::UtcNow -ge $deadline) { throw 'Runner did not become ready.' }
    & (Join-Path $PSScriptRoot 'Set-LegoInferenceControl.ps1') -Name $name -Action Pause -WaitPausedMs 5000
    Start-Sleep -Milliseconds 200
    if ($process.HasExited) { throw 'Runner exited while paused.' }
    & (Join-Path $PSScriptRoot 'Set-LegoInferenceControl.ps1') -Name $name -Action Resume
    $ack = [Threading.EventWaitHandle]::OpenExisting("Local\LEGO-$name-Paused")
    try {
        $deadline = [DateTime]::UtcNow.AddSeconds(5)
        while ($ack.WaitOne(0)) {
            if ([DateTime]::UtcNow -ge $deadline) { throw 'Resume was not acknowledged.' }
            Start-Sleep -Milliseconds 5
        }
    } finally { $ack.Dispose() }
    & (Join-Path $PSScriptRoot 'Set-LegoInferenceControl.ps1') -Name $name -Action Pause -WaitPausedMs 5000
    & (Join-Path $PSScriptRoot 'Set-LegoInferenceControl.ps1') -Name $name -Action Stop
    if (!$process.WaitForExit(30000)) { throw 'Stopping a paused runner did not finish within 30 seconds.' }
    $process.Refresh()
    if ($null -eq $process.ExitCode -or $process.ExitCode -ne 0) { throw "Runner exit code '$($process.ExitCode)' did not indicate success. Inspect $stderr." }
    $summary = Get-Content -LiteralPath (Join-Path $inferenceOutput 'summary.json') -Raw | ConvertFrom-Json
    if ($summary.status -ne 'stopped' -or $summary.pauses -lt 2 -or $summary.resumes -lt 2) { throw 'External pause/resume/stop sequence was not recorded correctly.' }
    Write-Host "External control verified, including stop while paused: $OutputDirectory"
} finally {
    if ($process -and !$process.HasExited) {
        try { & (Join-Path $PSScriptRoot 'Set-LegoInferenceControl.ps1') -Name $name -Action Stop } catch {}
        if (!$process.WaitForExit(5000)) { $process.Kill() }
    }
    if ($process) { $process.Dispose() }
}
