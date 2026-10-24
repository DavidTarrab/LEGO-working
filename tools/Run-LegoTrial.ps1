[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateSet('RenderOnly','LlmOnly','Unrestricted','InterFrame','IntraFrame')][string]$Mode = 'Unrestricted',
    [ValidateRange(1, 86400)][int]$Seconds = 120,
    [ValidateRange(1, 6000)][int]$APM = 200,
    [ValidateRange(1, 1000)][int]$FPS = 60,
    [ValidateRange(320, 7680)][int]$ResX = 1920,
    [ValidateRange(240, 4320)][int]$ResY = 1080,
    [string]$GameExe,
    [string]$Project,
    [string]$Purpose = 'benchmark',
    [switch]$Insights,
    [string]$PresentMon
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$GameExe) { $GameExe = Join-Path $env:LEGO_ENGINE_ROOT 'Engine\Binaries\Win64\UE4Editor.exe' }
if (!$PresentMon) { $PresentMon = $env:LEGO_PRESENTMON }
if (!$Project) { $Project = Join-Path $legoStorage 'DialogueUpstream\DialogueDemo.uproject' }
$GameExe = [IO.Path]::GetFullPath($GameExe)
$trial = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutputDirectory)) {$OutputDirectory} else {Join-Path $legoRoot $OutputDirectory}))
if (!(Test-Path -LiteralPath $GameExe)) { throw "Game binary not found: $GameExe" }
if (Test-Path -LiteralPath $trial) { throw 'Choose a new trial directory; existing results will not be overwritten.' }
if ($Mode -ne 'LlmOnly') {
    if (!(Test-Path -LiteralPath $PresentMon)) { throw "PresentMon not found: $PresentMon" }
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run this capture from an elevated PowerShell terminal so PresentMon can open its ETW session.' }
}
$null = New-Item -ItemType Directory -Path $trial
$policy = switch ($Mode) { 'InterFrame' {1} 'IntraFrame' {0} default {2} }
$arguments = @()
if ([IO.Path]::GetFileName($GameExe) -eq 'UE4Editor.exe') { $arguments += '"' + $Project + '"' }
$arguments += @('/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap?game=/Script/DialogueDemo.LegoExperimentGameMode',
    '-game', '-dx12', '-windowed', "-ResX=$ResX", "-ResY=$ResY", '-unattended', '-nosplash', '-nosound',
    '-LegoQuitAfterRun', "-LegoSeconds=$Seconds", "-LegoAPM=$APM", "-LegoFPS=$FPS", "-LegoPolicy=$policy",
    ('-LegoModel="' + $env:LEGO_MODEL_PATH + '"'),
    ('-LegoOutput="' + (Join-Path $trial 'llm') + '"'),
    ('-abslog="' + (Join-Path $trial 'game.log') + '"'),
    ('-UserDir="' + (Join-Path $trial 'userdata') + '"'), '-stdout', '-FullStdOutLogOutput')
if ($Mode -eq 'RenderOnly') { $arguments += @('-LegoRenderOnly', ('-LegoRenderOutput="' + (Join-Path $trial 'render') + '"')) }
else { $arguments += '-LegoAutoStart' }
if ($Mode -eq 'LlmOnly') { $arguments += '-nullrhi' }
if ($Insights) { $arguments += @('-trace=cpu,gpu,frame,bookmark', '-statnamedevents', ('-tracefile="' + (Join-Path $trial 'game.utrace') + '"')) }
$metadata = [ordered]@{ mode=$Mode; purpose=$Purpose; seconds=$Seconds; requested_apm=$APM; target_fps=$FPS; resolution=@($ResX,$ResY); game_binary=$GameExe; project=$Project;
    start_utc=[DateTime]::UtcNow.ToString('o'); gpu_utc_offset=[DateTimeOffset]::Now.ToString('zzz'); insights=[bool]$Insights;
    scene='/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap'; camera='template initial camera, no scripted movement'; focus='not monitored';
    backend_build=(Get-Content -LiteralPath (Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'build-info.json') -Raw | ConvertFrom-Json) }
$metadata.background_compilers = @(Get-Process -Name cl,link,UnrealBuildTool -ErrorAction SilentlyContinue | Select-Object ProcessName,Id)
$metadata.gpu = @(& nvidia-smi '--query-gpu=name,driver_version,memory.total' '--format=csv,noheader')
if ($LASTEXITCODE) { throw 'nvidia-smi query failed.' }
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $trial 'experiment.json') -Encoding UTF8
$gameProcess = $null; $capture = $null; $gpu = $null; $gpuStop = $null
try {
    $gpuStopName = 'Local\LEGO-GPU-' + [Guid]::NewGuid().ToString('N')
    $gpuStop = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$gpuStopName)
    $gpu = Start-Process -FilePath (Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'lego-gpu-sample.exe') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $trial 'gpu.stdout.log') -RedirectStandardError (Join-Path $trial 'gpu.stderr.log') -ArgumentList @(
        '--output',('"' + (Join-Path $trial 'gpu.csv') + '"'),'--stop-event',$gpuStopName,'--interval-ms','200')
    $gpuHandle = $gpu.Handle
    $style = if ($Mode -eq 'LlmOnly') {'Hidden'} else {'Normal'}
    $gameProcess = Start-Process -FilePath $GameExe -ArgumentList $arguments -WindowStyle $style -PassThru -RedirectStandardOutput (Join-Path $trial 'stdout.log') -RedirectStandardError (Join-Path $trial 'stderr.log')
    $gameHandle = $gameProcess.Handle
    if ($Mode -ne 'LlmOnly') {
        $capture = Start-Process -FilePath $PresentMon -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $trial 'presentmon.stdout.log') -RedirectStandardError (Join-Path $trial 'presentmon.stderr.log') -ArgumentList @(
            '-process_id', $gameProcess.Id, '-output_file', ('"' + (Join-Path $trial 'frames-raw.csv') + '"'), '-qpc_time',
            '-session_name', ('LEGO-' + [Guid]::NewGuid().ToString('N')), '-terminate_on_proc_exit', '-dont_restart_as_admin', '-no_top')
        $captureHandle = $capture.Handle
    }
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 300)
    while (!$gameProcess.WaitForExit(1000)) {
        if ($capture -and $capture.HasExited) { throw 'PresentMon exited before the game. Inspect presentmon.stderr.log; ETW capture may require elevation.' }
        if ([DateTime]::UtcNow -ge $deadline) { throw 'Game exceeded the measurement duration plus five minutes of startup/shutdown allowance.' }
    }
    $gameProcess.Refresh()
    if ($null -eq $gameProcess.ExitCode -or $gameProcess.ExitCode -ne 0) { throw "Game exit code '$($gameProcess.ExitCode)'; inspect game.log." }
    $null = $gpuStop.Set()
    if (!$gpu.WaitForExit(5000)) { throw 'GPU sampler did not stop cleanly.' }
    $gpu.Refresh()
    if ($null -eq $gpu.ExitCode -or $gpu.ExitCode -ne 0) { throw 'GPU sampling failed. Inspect gpu.stderr.log.' }
    if ($capture) {
        if (!$capture.WaitForExit(15000)) { throw 'PresentMon did not finish after game exit.' }
        $capture.Refresh()
        if ($null -eq $capture.ExitCode -or $capture.ExitCode -ne 0) { throw 'PresentMon failed; inspect presentmon.stderr.log.' }
    }
    if ($Mode -eq 'RenderOnly') { $measurement = Join-Path $trial 'render\measurement.json' }
    else {
        $summaries = @(Get-ChildItem -LiteralPath (Join-Path $trial 'llm') -Filter summary.json -Recurse -File)
        if ($summaries.Count -ne 1) { throw 'Expected one integrated inference run.' }
        $run = $summaries[0].DirectoryName
        $summary = Get-Content -LiteralPath $summaries[0].FullName -Raw | ConvertFrom-Json
        if (!$summary.success -or $summary.actions -lt 1) { throw "Inference run did not finish successfully: $($summary.error)" }
        $measurement = Join-Path $run 'measurement.json'
        Copy-Item -LiteralPath (Join-Path $trial 'gpu.csv') -Destination (Join-Path $run 'gpu.csv')
    }
    if ($capture) {
        & node (Join-Path $PSScriptRoot 'Select-LegoMeasurement.mjs') (Join-Path $trial 'frames-raw.csv') $measurement (Join-Path $trial 'frames.csv')
        if ($LASTEXITCODE) { throw 'Frame-window selection failed.' }
    }
    if ($Mode -ne 'RenderOnly') {
        & node (Join-Path $PSScriptRoot 'Analyze-ConcurrentRun.mjs') $trial --fps $FPS --gpu-utc-offset $metadata.gpu_utc_offset
        if ($LASTEXITCODE) { throw 'Analysis failed.' }
    }
    Write-Host "Completed $Mode trial: $trial"
} finally {
    if ($gpuStop) { $null = $gpuStop.Set(); if ($gpu -and !$gpu.HasExited) { $null=$gpu.WaitForExit(5000) } }
    foreach ($child in @($gameProcess,$capture,$gpu)) {
        if ($child) { if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
    }
    if ($gpuStop) { $gpuStop.Dispose() }
}
