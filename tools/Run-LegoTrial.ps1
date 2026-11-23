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
    [switch]$SkipPresentMon,
    [switch]$PresentMonV2,
    [Alias('WprPresent')][switch]$WprGpu,
    [string]$WprProfile,
    [string[]]$ExtraGameArguments = @(),
    [string]$Nsight,
    [string]$PresentMon
)
$ErrorActionPreference = 'Stop'
if ($WprGpu -and ($SkipPresentMon -or !$PresentMonV2)) { throw 'WprGpu requires PresentMonV2 and cannot be combined with SkipPresentMon.' }
if ($GameExe -and ![IO.Path]::IsPathRooted($GameExe)) { $GameExe = Join-Path $PWD $GameExe }
if ($Project -and ![IO.Path]::IsPathRooted($Project)) { $Project = Join-Path $PWD $Project }
if ($PresentMon -and ![IO.Path]::IsPathRooted($PresentMon)) { $PresentMon = Join-Path $PWD $PresentMon }
if ($Nsight -and ![IO.Path]::IsPathRooted($Nsight)) { $Nsight = Join-Path $PWD $Nsight }
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$GameExe) { $GameExe = Join-Path $env:LEGO_ENGINE_ROOT 'Engine\Binaries\Win64\UE4Editor.exe' }
if (!$PresentMon) { $PresentMon = $env:LEGO_PRESENTMON }
if (!$Project) { $Project = Join-Path $legoStorage 'DialogueUpstream\DialogueDemo.uproject' }
$GameExe = [IO.Path]::GetFullPath($GameExe)
$trial = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutputDirectory)) {$OutputDirectory} else {Join-Path $legoRoot $OutputDirectory}))
if (Test-Path -LiteralPath (Join-Path (Split-Path -Parent $trial) 'STOP')) { throw 'Batch stop marker found; no new capture was started.' }
if (!(Test-Path -LiteralPath $GameExe)) { throw "Game binary not found: $GameExe" }
if (Test-Path -LiteralPath $trial) { throw 'Choose a new trial directory; existing results will not be overwritten.' }
if ($Mode -ne 'LlmOnly' -and !$SkipPresentMon) {
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
$arguments += $ExtraGameArguments
if (!('LegoWindowProbeV2' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LegoWindowProbeV2 {
    delegate bool WindowVisitor(IntPtr window, IntPtr data);
    [DllImport("user32.dll")] static extern bool EnumWindows(WindowVisitor visitor, IntPtr data);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr window, System.Text.StringBuilder name, int capacity);
    [DllImport("user32.dll")] static extern bool ShowWindowAsync(IntPtr window, int command);
    [DllImport("user32.dll")] static extern bool SetForegroundWindow(IntPtr window);
    [DllImport("user32.dll")] static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr window, out uint process);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr window);
    public static uint ForegroundProcess() { uint process; GetWindowThreadProcessId(GetForegroundWindow(), out process); return process; }
    public static bool ShowGameWindow(uint target) {
        bool found = false;
        EnumWindows((window,data) => {
            uint process; GetWindowThreadProcessId(window, out process);
            if (process != target) return true;
            var name = new System.Text.StringBuilder(256);
            GetClassName(window,name,256);
            if (name.ToString() != "UnrealWindow") return true;
            ShowWindowAsync(window,5); SetForegroundWindow(window); found=true; return false;
        },IntPtr.Zero);
        return found;
    }
}
'@
}
$metadata = [ordered]@{ mode=$Mode; purpose=$Purpose; seconds=$Seconds; requested_apm=$APM; target_fps=$FPS; resolution=@($ResX,$ResY); game_binary=$GameExe; project=$Project;
    start_utc=[DateTime]::UtcNow.ToString('o'); gpu_utc_offset=[DateTimeOffset]::Now.ToString('zzz'); insights=[bool]$Insights; presentmon_enabled=($Mode -ne 'LlmOnly' -and !$SkipPresentMon);
    scene='/Game/ThirdPersonBP/Maps/ThirdPersonExampleMap'; camera='template initial camera, no scripted movement'; focus='sampled at approximately 1 Hz in focus.csv'; extra_game_arguments=$ExtraGameArguments;
    backend_build=(Get-Content -LiteralPath (Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'build-info.json') -Raw | ConvertFrom-Json) }
$metadata.background_compilers = @(Get-Process -Name cl,link,UnrealBuildTool -ErrorAction SilentlyContinue | Select-Object ProcessName,Id)
$metadata.gpu = @(& nvidia-smi '--query-gpu=name,driver_version,memory.total' '--format=csv,noheader')
if ($LASTEXITCODE) { throw 'nvidia-smi query failed.' }
$metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $trial 'experiment.json') -Encoding UTF8
$gameProcess = $null; $profiler = $null; $capture = $null; $gpu = $null; $gpuStop = $null; $captureSession = $null; $wprStarted = $false
$focusRows = [Collections.Generic.List[string]]::new()
$focusRows.Add('time_utc,game_is_foreground,game_is_minimized')
try {
    $gpuStopName = 'Local\LEGO-GPU-' + [Guid]::NewGuid().ToString('N')
    $gpuStop = [Threading.EventWaitHandle]::new($false,[Threading.EventResetMode]::ManualReset,$gpuStopName)
    $gpu = Start-Process -FilePath (Join-Path $env:LEGO_UPSTREAM_BACKEND_DIR 'lego-gpu-sample.exe') -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $trial 'gpu.stdout.log') -RedirectStandardError (Join-Path $trial 'gpu.stderr.log') -ArgumentList @(
        '--output',('"' + (Join-Path $trial 'gpu.csv') + '"'),'--stop-event',$gpuStopName,'--interval-ms','200')
    $gpuHandle = $gpu.Handle
    $style = if ($Mode -eq 'LlmOnly') {'Hidden'} else {'Normal'}
    if ($Mode -ne 'LlmOnly' -and !$SkipPresentMon -and $WprGpu) {
        if (!$WprProfile) { $WprProfile = (Join-Path $PSScriptRoot 'LegoPresent.wprp') + '!LegoPresent' }
        $wprStatus = (& wpr -status 2>&1 | Out-String)
        $wprStatus | Set-Content (Join-Path $trial 'wpr-initial-status.log')
        if ($wprStatus -notmatch 'WPR is not recording') { throw 'WPR is already recording or its status is unrecognized; refusing to change another capture.' }
        $wprTemp = Join-Path $trial 'wpr-temp'
        $null = New-Item -ItemType Directory -Path $wprTemp
        & wpr -start $WprProfile -filemode -recordtempto $wprTemp *> (Join-Path $trial 'wpr-start.log')
        if ($LASTEXITCODE) { throw 'WPR GPU capture failed to start.' }
        $wprStarted = $true
    }
    if ($Mode -ne 'LlmOnly' -and !$SkipPresentMon -and !$WprGpu) {
        if (Get-Process -Name ([IO.Path]::GetFileNameWithoutExtension($GameExe)) -ErrorAction SilentlyContinue) { throw 'Close other instances of the target executable before capture.' }
        $captureSession = 'LEGO-' + [Guid]::NewGuid().ToString('N')
        $captureArguments = @(
            '-process_name', [IO.Path]::GetFileName($GameExe), '-output_file', ('"' + (Join-Path $trial 'frames-raw.csv') + '"'), '-qpc_time',
            '-session_name', $captureSession, '-dont_restart_as_admin', '-no_top')
        if ($PresentMonV2) {
            $captureArguments = @('--process_name',[IO.Path]::GetFileName($GameExe),'--output_file',('"' + (Join-Path $trial 'frames-raw.csv') + '"'),
                '--qpc_time','--session_name',$captureSession,'--v1_metrics','--no_console_stats','--no_track_gpu','--no_track_display','--no_track_input')
        }
        $metadata.presentmon_v2 = [bool]$PresentMonV2
        $metadata.presentmon_binary = $PresentMon
        $capture = Start-Process -FilePath $PresentMon -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $trial 'presentmon.stdout.log') -RedirectStandardError (Join-Path $trial 'presentmon.stderr.log') -ArgumentList $captureArguments
        $captureHandle = $capture.Handle
        Start-Sleep -Seconds 2
        if ($capture.HasExited) { throw 'PresentMon failed before game startup; inspect its logs.' }
    }
    if ($Nsight) {
        $targetName = [IO.Path]::GetFileNameWithoutExtension($GameExe)
        if (Get-Process -Name $targetName -ErrorAction SilentlyContinue) { throw 'Close other target processes before Nsight launch.' }
        $profileArguments = @('profile','--trace=cuda,nvtx,dx12,wddm','--sample=none','--cpuctxsw=none','--export=sqlite',
            ('--output="' + (Join-Path $trial 'gpu-timeline') + '"'),('"'+$GameExe+'"')) + $arguments
        $profiler = Start-Process -FilePath $Nsight -ArgumentList $profileArguments -WindowStyle Hidden -PassThru -RedirectStandardOutput (Join-Path $trial 'profiler.stdout.log') -RedirectStandardError (Join-Path $trial 'profiler.stderr.log')
        $profilerHandle = $profiler.Handle
        $launchDeadline = [DateTime]::UtcNow.AddSeconds(45)
        do {
            Start-Sleep -Milliseconds 100
            $targets = @(Get-Process -Name $targetName -ErrorAction SilentlyContinue)
            if ($targets.Count -gt 1) { throw 'Ambiguous Nsight target process.' }
            if ($profiler.HasExited) { throw 'Nsight exited before game launch; inspect profiler logs.' }
        } while (!$targets.Count -and [DateTime]::UtcNow -lt $launchDeadline)
        if (!$targets.Count) { throw 'Nsight did not launch the game within 45 seconds.' }
        $gameProcess = $targets[0]
        # The hidden profiler launcher can pass SW_HIDE to its child game.
        # Show only the target's Unreal window; leave profiler consoles hidden.
        do {
            if ([LegoWindowProbeV2]::ShowGameWindow($gameProcess.Id)) { break }
            Start-Sleep -Milliseconds 100
        } while (!$gameProcess.HasExited -and [DateTime]::UtcNow -lt $launchDeadline)
    } else {
        $gameProcess = Start-Process -FilePath $GameExe -ArgumentList $arguments -WindowStyle $style -PassThru -RedirectStandardOutput (Join-Path $trial 'stdout.log') -RedirectStandardError (Join-Path $trial 'stderr.log')
    }
    $gameHandle = $gameProcess.Handle
    if ($Mode -ne 'LlmOnly' -and !$Nsight) {
        $windowDeadline = [DateTime]::UtcNow.AddSeconds(10)
        do {
            if ([LegoWindowProbeV2]::ShowGameWindow($gameProcess.Id)) { break }
            Start-Sleep -Milliseconds 100
        } while (!$gameProcess.HasExited -and [DateTime]::UtcNow -lt $windowDeadline)
    }
    $metadata.game_process_id = $gameProcess.Id
    $metadata.presentmon_session = $captureSession
    $metadata.nsight_binary = $Nsight
    $metadata.wpr_capture = [bool]$WprGpu
    $metadata.wpr_profile = $WprProfile
    if ($WprGpu -or $PresentMonV2) { $metadata.presentmon_tracking = 'presentation intervals only; GPU/display completion and input tracking disabled' }
    $metadata.presentmon_binary = $PresentMon
    $metadata.presentmon_v2 = [bool]$PresentMonV2
    $metadata | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $trial 'experiment.json') -Encoding UTF8
    $deadline = [DateTime]::UtcNow.AddSeconds($Seconds + 300)
    while (!$gameProcess.WaitForExit(1000)) {
        if (Test-Path -LiteralPath (Join-Path $trial 'CANCEL')) { throw 'Owned trial cancellation requested; cleaning up game and capture processes.' }
        $gameProcess.Refresh()
        $focusRows.Add(('{0},{1},{2}' -f [DateTime]::UtcNow.ToString('o'),([LegoWindowProbeV2]::ForegroundProcess() -eq $gameProcess.Id),([LegoWindowProbeV2]::IsIconic($gameProcess.MainWindowHandle))))
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
        & logman stop $captureSession -ets *> (Join-Path $trial 'presentmon-stop.log')
        if ($LASTEXITCODE) { throw 'Could not stop the owned PresentMon ETW session cleanly.' }
        $captureSession = $null
        if (!$capture.WaitForExit(15000)) { throw 'PresentMon did not finish after game exit.' }
        $capture.Refresh()
        if ($null -eq $capture.ExitCode -or $capture.ExitCode -ne 0) { throw 'PresentMon failed; inspect presentmon.stderr.log.' }
    }
    if ($wprStarted) {
        & wpr -stop (Join-Path $trial 'gpu.etl') *> (Join-Path $trial 'wpr-stop.log')
        if ($LASTEXITCODE) { throw 'WPR GPU capture failed to stop.' }
        $wprStarted = $false
        & $PresentMon --etl_file (Join-Path $trial 'gpu.etl') --v1_metrics --qpc_time --output_file (Join-Path $trial 'frames-raw.csv') --no_console_stats --no_track_gpu --no_track_display --no_track_input *> (Join-Path $trial 'presentmon-offline.log')
        if ($LASTEXITCODE) { throw 'PresentMon offline decoding failed.' }
    }
    if ($profiler) {
        if (!$profiler.WaitForExit(120000)) { throw 'Nsight report export did not finish within two minutes.' }
        $profiler.Refresh()
        if ($profiler.ExitCode -ne 0) { throw 'Nsight failed; inspect profiler logs.' }
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
    if ($capture -or ($WprGpu -and $Mode -ne 'LlmOnly')) {
        & node (Join-Path $PSScriptRoot 'Select-LegoMeasurement.mjs') (Join-Path $trial 'frames-raw.csv') $measurement (Join-Path $trial 'frames.csv') $gameProcess.Id
        if ($LASTEXITCODE) { throw 'Frame-window selection failed.' }
    }
    if ($Mode -ne 'RenderOnly') {
        & node (Join-Path $PSScriptRoot 'Analyze-ConcurrentRun.mjs') $trial --fps $FPS --gpu-utc-offset $metadata.gpu_utc_offset
        if ($LASTEXITCODE) { throw 'Analysis failed.' }
    }
    Write-Host "Completed $Mode trial: $trial"
} finally {
    if ($wprStarted) { & wpr -stop (Join-Path $trial 'failed-gpu.etl') *> (Join-Path $trial 'wpr-cleanup.log') }
    if ($captureSession) { & logman stop $captureSession -ets *> (Join-Path $trial 'presentmon-cleanup.log') }
    $focusRows | Set-Content -LiteralPath (Join-Path $trial 'focus.csv') -Encoding UTF8
    if ($gpuStop) { $null = $gpuStop.Set(); if ($gpu -and !$gpu.HasExited) { $null=$gpu.WaitForExit(5000) } }
    foreach ($child in @($gameProcess,$profiler,$capture,$gpu)) {
        if ($child) { if (!$child.HasExited) { $child.Kill(); $child.WaitForExit() }; $child.Dispose() }
    }
    if ($gpuStop) { $gpuStop.Dispose() }
}
