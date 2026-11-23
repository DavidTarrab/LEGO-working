[CmdletBinding()]
param(
    [ValidateRange(10,600)][int]$Seconds = 30,
    [ValidateRange(1,6000)][int]$APM = 20,
    [ValidateRange(1,5)][int]$Repetitions = 2,
    [string]$OutputDirectory,
    [string]$ValidationDirectory,
    [string]$GameExe
)
$ErrorActionPreference = 'Stop'
$identity = [Security.Principal.WindowsIdentity]::GetCurrent()
if (!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this helper in Administrator PowerShell: Windows ETW frame capture requires elevation.'
}
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
. (Join-Path $source 'tools\Enter-LegoDllmEnvironment.ps1')
$storage = $legoStorage
$env:GIT_CONFIG_COUNT = '1'
$env:GIT_CONFIG_KEY_0 = 'safe.directory'
$env:GIT_CONFIG_VALUE_0 = $source.Replace('\','/')
if (!$ValidationDirectory) { throw 'Provide the completed clean validation batch directory.' }
$validation = Join-Path $ValidationDirectory 'status.json'
$ready = Get-Content -LiteralPath $validation -Raw | ConvertFrom-Json
if ($ready.success -ne $true) { throw 'The clean correctness/profile batch is not complete; do not overlap its timings.' }
if (!$OutputDirectory) { $OutputDirectory = Join-Path (Split-Path $storage) ('runs\dllm-20261123\frames-' + [DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss')) }
if (Test-Path -LiteralPath $OutputDirectory) { throw 'Choose a new capture batch directory.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory
$game = if ($GameExe) { [IO.Path]::GetFullPath($GameExe) } else { Join-Path $storage 'packaged-lego\WindowsNoEditor\DialogueDemo\Binaries\Win64\DialogueDemo.exe' }
$presentMon = Join-Path $storage 'tools\PresentMon-2.6.0-x64.exe'
foreach ($required in @($game, $presentMon, (Join-Path $storage 'llama-bin\llama-server.exe'), (Join-Path $storage 'models\model.gguf'))) {
    if (!(Test-Path -LiteralPath $required)) { throw "Capture prerequisite missing: $required" }
}
$server = $null
$client = $null
$completed = @()
try {
    foreach ($architecture in @('diffusion','autoregressive')) {
        $serverDir = Join-Path $OutputDirectory $architecture
        $null = New-Item -ItemType Directory -Path $serverDir
        if ($architecture -eq 'diffusion') {
            $executable = $env:LEGO_PYTHON
            $arguments = @('-u',('"'+(Join-Path $source 'dllm\dllm_server.py')+'"'),'--cache-dir',('"'+(Join-Path $env:HF_HOME 'hub')+'"'),'--local-files-only','--steps','4','--port','8087')
        } else {
            $executable = Join-Path $storage 'llama-bin\llama-server.exe'
            $arguments = @('-m',('"'+(Join-Path $storage 'models\model.gguf')+'"'),'-ngl','99','-c','2048','-np','1','--host','127.0.0.1','--port','8087')
        }
        if (Get-NetTCPConnection -LocalPort 8087 -State Listen -ErrorAction SilentlyContinue) { throw 'Port 8087 is in use; refusing to reuse an unrelated server.' }
        @{architecture=$architecture; executable=$executable; executable_sha256=(Get-FileHash -LiteralPath $executable).Hash;
          arguments=$arguments; interpretation='preliminary unrestricted system comparison, not an architecture-only or scheduled result'} |
            ConvertTo-Json -Depth 8 | Set-Content (Join-Path $serverDir 'server-launch.json') -Encoding UTF8
        $server = Start-Process -FilePath $executable -WindowStyle Hidden -PassThru -ArgumentList $arguments `
            -RedirectStandardOutput (Join-Path $serverDir 'server.stdout.log') -RedirectStandardError (Join-Path $serverDir 'server.stderr.log')
        $null = $server.Handle
        $deadline = [DateTime]::UtcNow.AddSeconds(180)
        $healthy = $false
        while ([DateTime]::UtcNow -lt $deadline -and !$server.HasExited) {
            try { $health = Invoke-RestMethod http://127.0.0.1:8087/health -TimeoutSec 2; $healthy = $health.status -eq 'ok' } catch { $healthy = $false }
            if ($healthy) { break }
            Start-Sleep -Seconds 1
        }
        if (!$healthy) { throw 'Owned model server failed readiness; inspect retained logs.' }
        $health | ConvertTo-Json -Depth 20 | Set-Content (Join-Path $serverDir 'server-health.json') -Encoding UTF8
        $only = Join-Path $serverDir 'model-only'
        $clientArgs = @(('"'+(Join-Path $source 'dllm\measure_http.py')+'"'),'--server','http://127.0.0.1:8087','--output-dir',('"'+$only+'"'),'--seconds',$Seconds,'--apm',$APM,'--tokens','16')
        if ($architecture -eq 'diffusion') { $clientArgs += @('--steps','4') }
        $client = Start-Process -FilePath $env:LEGO_PYTHON -WindowStyle Hidden -PassThru -ArgumentList $clientArgs `
            -RedirectStandardOutput (Join-Path $serverDir 'model-only.stdout.log') -RedirectStandardError (Join-Path $serverDir 'model-only.stderr.log')
        $null = $client.Handle
        $client.WaitForExit()
        $client.Refresh()
        if ($client.ExitCode -ne 0) { throw 'Model-only measurement failed.' }
        $client.Dispose()
        $client = $null
        for ($repeat=1; $repeat -le $Repetitions; $repeat++) {
            if (Test-Path -LiteralPath (Join-Path $OutputDirectory 'STOP')) { throw 'STOP marker: current completed trials retained.' }
            $trialRoot = Join-Path $serverDir ('concurrent-{0:d2}' -f $repeat)
            $options = @{OutputDirectory=$trialRoot; GameExe=$game; ServerUri='http://127.0.0.1:8087'; Seconds=$Seconds;
                APM=$APM; Tokens=16; PresentMon=$presentMon; Purpose='preliminary paired unrestricted contention'}
            if ($architecture -eq 'diffusion') { $options.DiffusionSteps=4 }
            & (Join-Path $source 'tools\Run-LegoExternalTrial.ps1') @options
            $completed += @{architecture=$architecture; repetition=$repeat; path=$trialRoot}
            @{success=$null; completed=$completed} | ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
            $trialMeta = Get-Content (Join-Path $trialRoot 'trial\experiment.json') -Raw | ConvertFrom-Json
            & node (Join-Path $source 'tools\Analyze-ConcurrentRun.mjs') (Join-Path $trialRoot 'trial') --fps 60 --llm-baseline $only --gpu-utc-offset $trialMeta.gpu_utc_offset
            if ($LASTEXITCODE) { throw 'Baseline comparison report failed.' }
        }
        Stop-Process -Id $server.Id
        $server.WaitForExit()
        $server.Dispose()
        $server = $null
    }
    & (Join-Path $source 'tools\Run-LegoTrial.ps1') -Mode RenderOnly -GameExe $game -Seconds $Seconds `
        -OutputDirectory (Join-Path $OutputDirectory 'render-only') -PresentMon $presentMon -PresentMonV2 -WprPresent -Purpose 'matched preliminary rendering baseline'
    foreach ($item in $completed) {
        $trialPath = Join-Path $item.path 'trial'
        $trialMeta = Get-Content (Join-Path $trialPath 'experiment.json') -Raw | ConvertFrom-Json
        & node (Join-Path $source 'tools\Analyze-ConcurrentRun.mjs') $trialPath --fps 60 `
            --llm-baseline (Join-Path $OutputDirectory ($item.architecture + '\model-only')) `
            --game-baseline (Join-Path $OutputDirectory 'render-only') --gpu-utc-offset $trialMeta.gpu_utc_offset
        if ($LASTEXITCODE) { throw 'Matched model/render baseline report failed.' }
    }
    @{success=$true; completed=$completed; paper_ready=$false;
      reason='short preliminary workload, sequential model order; inspect focus/capture eligibility and quality before use'} |
        ConvertTo-Json -Depth 8 | Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
} catch {
    @{success=$false; completed=$completed; error=$_.Exception.Message} | ConvertTo-Json -Depth 8 |
        Set-Content (Join-Path $OutputDirectory 'status.json') -Encoding UTF8
    throw
} finally {
    if ($client) { if (!$client.HasExited) { Stop-Process -Id $client.Id }; $client.Dispose() }
    if ($server) { if (!$server.HasExited) { Stop-Process -Id $server.Id }; $server.Dispose() }
}
