[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$OutputDirectory,
    [ValidateRange(1,86400)][int]$Seconds = 120,
    [ValidateRange(1,6000)][int]$APM = 200,
    [ValidateRange(1,1000)][int]$FPS = 60,
    [ValidateRange(1,20)][int]$Repetitions = 3,
    [string]$GameExe,
    [string]$Purpose = 'controlled comparison'
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Enter-LegoEnvironment.ps1')
if (!$GameExe) { $GameExe = Join-Path $legoStorage 'packaged-lego\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe' }
$root = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($OutputDirectory)) {$OutputDirectory} else {Join-Path $legoRoot $OutputDirectory}))
if (Test-Path -LiteralPath $root) { throw 'Choose a new matrix directory; existing results will not be overwritten.' }
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'Run the matrix from an elevated PowerShell terminal for PresentMon capture.' }
$null = New-Item -ItemType Directory -Path $root
$records = @()
try {
    for ($repeat=1; $repeat -le $Repetitions; $repeat++) {
        # Rotate policy order between repetitions; baselines are repeated each time.
        $policies = @('Unrestricted','InterFrame','IntraFrame')
        $shift = ($repeat-1) % 3
        $order = @('RenderOnly','LlmOnly') + @($policies[$shift..2])
        if ($shift -gt 0) { $order += $policies[0..($shift-1)] }
        $group = Join-Path $root ('repeat-{0:d2}' -f $repeat)
        foreach ($mode in $order) {
            $trial = Join-Path $group $mode
            @{success=$null; current_mode=$mode; repetition=$repeat; completed=$records} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'status.json') -Encoding UTF8
            & (Join-Path $PSScriptRoot 'Run-LegoTrial.ps1') -OutputDirectory $trial -Mode $mode -Seconds $Seconds -APM $APM -FPS $FPS -GameExe $GameExe -Purpose $Purpose *> (Join-Path $root ("repeat-$repeat-$mode.log"))
            $records += [ordered]@{repetition=$repeat; mode=$mode; trial=$trial}
        }
        foreach ($mode in $policies) {
            $trial = Join-Path $group $mode
            $metadata = Get-Content -LiteralPath (Join-Path $trial 'experiment.json') -Raw | ConvertFrom-Json
            & node (Join-Path $PSScriptRoot 'Analyze-ConcurrentRun.mjs') $trial --fps $FPS --gpu-utc-offset $metadata.gpu_utc_offset --llm-baseline (Join-Path $group 'LlmOnly') --game-baseline (Join-Path $group 'RenderOnly')
            if ($LASTEXITCODE) { throw "Baseline comparison failed for $trial" }
        }
    }
    @{success=$true; purpose=$Purpose; seconds=$Seconds; requested_apm=$APM; target_fps=$FPS; completed=$records} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'status.json') -Encoding UTF8
    & node (Join-Path $PSScriptRoot 'Summarize-LegoMatrix.mjs') $root
    if ($LASTEXITCODE) { throw 'Matrix summary failed.' }
} catch {
    @{success=$false; error=$_.Exception.Message; completed=$records} | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $root 'status.json') -Encoding UTF8
    throw
}
