[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [Parameter(Mandatory)][string]$GameExe,
    [string]$ServerUri = 'http://127.0.0.1:8080',
    [ValidateRange(1,3600)][int]$Seconds = 30,
    [ValidateRange(1,6000)][int]$APM = 100,
    [ValidateRange(1,2048)][int]$Tokens = 16,
    [ValidateRange(0,2048)][int]$DiffusionSteps = 0,
    [string]$PresentMon,
    [switch]$SkipPresentMon,
    [string]$Purpose = 'preliminary external contention validation'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoDllmEnvironment.ps1')
$root = [IO.Path]::GetFullPath($OutputDirectory)
$GameExe = [IO.Path]::GetFullPath($GameExe)
if (Test-Path -LiteralPath $root) { throw 'Choose a new output directory; failed trials are retained.' }
if (!(Test-Path -LiteralPath $GameExe)) { throw 'Game binary missing.' }
if (!$SkipPresentMon) {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    if (!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Windows ETW frame capture requires an Administrator PowerShell terminal.'
    }
}
if (!$PresentMon) { $PresentMon = Join-Path $legoStorage 'tools\PresentMon-2.6.0-x64.exe' }
$ServerUri = $ServerUri.TrimEnd('/')
$health = Invoke-RestMethod "$ServerUri/health" -TimeoutSec 15
if ($health.status -ne 'ok') { throw 'Server not ready.' }
$body = @{prompt='You control a fighter. Health 60, enemy health 40, distance 3 meters. Available actions: dodge, attack, retreat. Choose an action and explain briefly.';
    n_predict=$Tokens; temperature=0; seed=1; stream=$false; cache_prompt=$false}
if ($DiffusionSteps) { $body.steps = $DiffusionSteps }
# Warmup must finish before the game publishes its measurement start.
$null = Invoke-RestMethod "$ServerUri/completion" -Method Post -ContentType 'application/json' -Body ($body | ConvertTo-Json) -TimeoutSec 300
$null = New-Item -ItemType Directory -Path $root
$trial = Join-Path $root 'trial'
$run = Join-Path $trial 'llm\external'
$gameProcess = $null
$clientProcess = $null
try {
    $launch = @('-NoProfile','-ExecutionPolicy','Bypass','-File',('"'+(Join-Path $PSScriptRoot 'Run-LegoTrial.ps1')+'"'),
        '-OutputDirectory',('"'+$trial+'"'),'-GameExe',('"'+$GameExe+'"'),'-Mode','RenderOnly',
        '-Seconds',($Seconds+60),'-APM',$APM,'-PresentMon',('"'+$PresentMon+'"'),'-Purpose',('"'+$Purpose+'"'))
    if ($SkipPresentMon) { $launch += '-SkipPresentMon' }
    else { $launch += @('-PresentMonV2','-WprPresent') }
    $gameProcess = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') -WindowStyle Hidden -PassThru -ArgumentList $launch `
        -RedirectStandardOutput (Join-Path $root 'game-runner.stdout.log') -RedirectStandardError (Join-Path $root 'game-runner.stderr.log')
    $null = $gameProcess.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while (!(Test-Path -LiteralPath $trial)) {
        if ($gameProcess.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Game runner failed before creating capture directory.' }
        Start-Sleep -Milliseconds 50
    }
    $clientArgs = @(('"'+(Join-Path $legoRoot 'dllm\measure_http.py')+'"'), '--server',$ServerUri,
        '--output-dir',('"'+$run+'"'),'--seconds',$Seconds,'--apm',$APM,'--tokens',$Tokens,'--warmup','0',
        '--window-file',('"'+(Join-Path $trial 'render\measurement.json')+'"'))
    if ($DiffusionSteps) { $clientArgs += @('--steps',$DiffusionSteps) }
    $clientProcess = Start-Process -FilePath $env:LEGO_PYTHON -WindowStyle Hidden -PassThru -ArgumentList $clientArgs `
        -RedirectStandardOutput (Join-Path $root 'client.stdout.log') -RedirectStandardError (Join-Path $root 'client.stderr.log')
    $null = $clientProcess.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 300)
    while (!$clientProcess.WaitForExit(1000)) {
        if ($gameProcess.HasExited -or [DateTime]::UtcNow -ge $deadline) { throw 'Game ended or client timed out before request collection finished.' }
    }
    $clientProcess.Refresh()
    if ($clientProcess.ExitCode -ne 0) { throw 'External client failed; inspect client.stderr.log.' }
    while (!$gameProcess.WaitForExit(1000)) {
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Game/capture runner timed out.' }
    }
    $gameProcess.Refresh()
    if ($gameProcess.ExitCode -ne 0) { throw 'Game/capture runner failed; inspect runner logs.' }
    $window = Get-Content (Join-Path $run 'measurement.json') -Raw | ConvertFrom-Json
    $gameWindow = Get-Content (Join-Path $trial 'render\measurement.json') -Raw | ConvertFrom-Json
    if (!$window.complete -or !$gameWindow.complete -or
        [Int64]$window.start_qpc_ticks -lt [Int64]$gameWindow.start_qpc_ticks -or
        [Int64]$window.end_qpc_ticks -gt [Int64]$gameWindow.end_qpc_ticks) { throw 'Game did not cover the complete inference interval and drain.' }
    Copy-Item -LiteralPath (Join-Path $trial 'gpu.csv') -Destination (Join-Path $run 'gpu.csv')
    $meta = Get-Content (Join-Path $trial 'experiment.json') -Raw | ConvertFrom-Json
    $meta.mode = 'ExternalUnrestricted'
    $meta | Add-Member -NotePropertyName external_server_health -NotePropertyValue $health
    $meta | Add-Member -NotePropertyName game_sha256 -NotePropertyValue (Get-FileHash -LiteralPath $GameExe -Algorithm SHA256).Hash
    $meta | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $trial 'experiment.json') -Encoding UTF8
    if (!$SkipPresentMon) {
        & node (Join-Path $PSScriptRoot 'Select-LegoMeasurement.mjs') (Join-Path $trial 'frames-raw.csv') (Join-Path $run 'measurement.json') (Join-Path $trial 'frames.csv') $meta.game_process_id
        if ($LASTEXITCODE) { throw 'Inference-window frame crop failed.' }
    }
    & node (Join-Path $PSScriptRoot 'Analyze-ConcurrentRun.mjs') $trial --fps 60 --gpu-utc-offset $meta.gpu_utc_offset
    if ($LASTEXITCODE) { throw 'External trial analysis failed.' }
    $start = [DateTimeOffset]::Parse($window.start_utc)
    $end = $start.AddSeconds(([Int64]$window.end_qpc_ticks - [Int64]$window.start_qpc_ticks) / $window.qpc_frequency)
    $focus = @(Import-Csv (Join-Path $trial 'focus.csv') | Where-Object {
        $stamp=[DateTimeOffset]::Parse($_.time_utc); $stamp -ge $start.AddSeconds(-2) -and $stamp -le $end })
    $focusValid = $focus.Count -ge 3 -and @($focus | Where-Object { $_.game_is_foreground -ne 'True' -or $_.game_is_minimized -ne 'False' }).Count -eq 0
    @{success=$true; capture_valid=(-not $SkipPresentMon); sampled_focus_valid=$focusValid;
      valid_for_frame_comparison=(!$SkipPresentMon -and $focusValid); purpose=$Purpose;
      interpretation='unrestricted co-location; not game-controlled diffusion scheduling'} |
        ConvertTo-Json | Set-Content (Join-Path $root 'status.json') -Encoding UTF8
} catch {
    @{success=$false; error=$_.Exception.Message; purpose=$Purpose} | ConvertTo-Json |
        Set-Content (Join-Path $root 'status.json') -Encoding UTF8
    throw
} finally {
    if ($clientProcess) {
        if (!$clientProcess.HasExited) { Stop-Process -Id $clientProcess.Id }
        $clientProcess.Dispose()
    }
    if ($gameProcess) {
        if (!$gameProcess.HasExited) {
            # Let the owning runner stop its own ETW session and child game.
            # Killing its PowerShell process would skip its cleanup block.
            if (Test-Path -LiteralPath $trial) { Set-Content -LiteralPath (Join-Path $trial 'CANCEL') -Value 'external trial cancelled' }
            if (!$gameProcess.WaitForExit(15000)) { Write-Warning "Capture runner PID $($gameProcess.Id) is still performing cleanup; inspect its logs." }
        }
        $gameProcess.Dispose()
    }
}
