[CmdletBinding()]
param(
    [string]$ServerUri = 'http://127.0.0.1:8080',
    [ValidateRange(1, 86400)][int]$Seconds = 120,
    [ValidateRange(1, 6000)][int]$APM = 200,
    [ValidateRange(1, 2048)][int]$Tokens = 16,
    [string]$OutputDir = (Join-Path $PSScriptRoot '../runs'),
    [string]$Prompt = 'You control a fighter. Health 60, enemy health 40, distance 3 meters. Available actions: dodge, attack, retreat. Choose an action and explain briefly.',
    [switch]$SkipGpuSampling
)

# A separate-process contention baseline; this does not enable LEGO scheduling.
$ErrorActionPreference = 'Stop'
$ServerUri = $ServerUri.TrimEnd('/')
$health = Invoke-RestMethod "$ServerUri/health" -TimeoutSec 10
if ($health.status -ne 'ok') { throw 'llama-server is not ready.' }
$runDir = Join-Path $OutputDir ([DateTime]::UtcNow.ToString('yyyyMMdd-HHmmss-fff'))
$null = New-Item -ItemType Directory -Path $runDir -Force
$runDir = (Resolve-Path -LiteralPath $runDir).Path
$csvPath = Join-Path $runDir 'llm.csv'
@{ server = $ServerUri; seconds = $Seconds; requested_apm = $APM; tokens = $Tokens;
   prompt = $Prompt; start_utc = [DateTime]::UtcNow.ToString('o'); baseline = 'external-process';
   cache_prompt = $false; parallel_requests = 1; server_health = $health;
   timing_source = $(if ($health.timing_source) { $health.timing_source } else { 'server-reported' });
   latency_origin = 'scheduled-arrival' } |
    ConvertTo-Json -Depth 16 | Set-Content -LiteralPath (Join-Path $runDir 'config.json') -Encoding UTF8

$sampler = $null
$samples = New-Object 'System.Collections.Generic.List[double]'
$count = 0
$missed = 0
$deadlineMisses = 0
try {
    if (!$SkipGpuSampling) {
        $smi = (Get-Command nvidia-smi -ErrorAction Stop).Source
        $gpuPath = Join-Path $runDir 'gpu.csv'
        $sampler = Start-Process -FilePath $smi -WindowStyle Hidden -PassThru -ArgumentList @(
            '--query-gpu=timestamp,index,utilization.gpu,memory.used,memory.total,power.draw',
            '--format=csv', '-lms', '500', '-f', ('"' + $gpuPath + '"'))
    }
    # Warm up the model before recording requests.
    $body = @{ prompt = $Prompt; n_predict = $Tokens; temperature = 0;
        seed = 1; cache_prompt = $false; stream = $false } | ConvertTo-Json
    $null = Invoke-RestMethod "$ServerUri/completion" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 300
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $originUtc = [DateTime]::UtcNow
    $startQpc = [Diagnostics.Stopwatch]::GetTimestamp()
    $periodMs = 60000.0 / $APM
    $nextMs = 0.0
    while ($clock.Elapsed.TotalSeconds -lt $Seconds) {
        $waitMs = $nextMs - $clock.Elapsed.TotalMilliseconds
        if ($waitMs -gt 0) { Start-Sleep -Milliseconds ([int][Math]::Ceiling($waitMs)) }
        if ($clock.Elapsed.TotalSeconds -ge $Seconds) { break }
        $startUtc = $originUtc.AddMilliseconds($nextMs).ToString('o')
        $startMs = $clock.Elapsed.TotalMilliseconds
        $response = Invoke-RestMethod "$ServerUri/completion" -Method Post -ContentType 'application/json' -Body $body -TimeoutSec 300
        $completionMs = $clock.Elapsed.TotalMilliseconds
        $latencyMs = $completionMs - $nextMs
        $serviceMs = $completionMs - $startMs
        if ($latencyMs -gt $periodMs) { $deadlineMisses++ }
        @{action=($count + 1); response=$response} | ConvertTo-Json -Depth 24 -Compress |
            Add-Content -LiteralPath (Join-Path $runDir 'responses.jsonl') -Encoding UTF8
        $samples.Add($latencyMs)
        $count++
        [pscustomobject]@{
            action = $count; start_utc = $startUtc; start_elapsed_ms = $nextMs;
            latency_ms = $latencyMs; deadline_ms = $periodMs; deadline_missed = ($latencyMs -gt $periodMs);
            service_ms = $serviceMs; arrival_lateness_ms = ($startMs - $nextMs);
            prompt_tokens = $response.timings.prompt_n; prompt_ms = $response.timings.prompt_ms;
            generated_tokens = $response.timings.predicted_n; decode_ms = $response.timings.predicted_ms;
            decode_tokens_per_second = $response.timings.predicted_per_second;
            diffusion_steps = $response.diffusion.steps_requested;
            executed_forwards = $response.diffusion.executed_forwards;
            admission_wait_ms = $response.diffusion.admission_wait_ms
        } | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Append -Encoding UTF8
        $nextMs += $periodMs
        # Skip arrivals missed while busy instead of building an unbounded request queue.
        while ($nextMs -lt $clock.Elapsed.TotalMilliseconds -and $nextMs -lt 1000.0 * $Seconds) { $nextMs += $periodMs; $missed++ }
    }
    $clock.Stop()
    if ($count -gt 0) {
        $sorted = @($samples | Sort-Object)
        $p99 = $sorted[[Math]::Ceiling(0.99 * $count) - 1]
        [pscustomobject]@{ actions = $count; elapsed_seconds = $clock.Elapsed.TotalSeconds;
            achieved_apm = 60.0 * $count / $clock.Elapsed.TotalSeconds;
            skipped_arrivals = $missed; offered_arrivals = [Math]::Ceiling($Seconds * $APM / 60.0);
            deadline_misses = $deadlineMisses;
            on_time_offered_fraction = ($count - $deadlineMisses) / [Math]::Ceiling($Seconds * $APM / 60.0);
            measurement_window_seconds = $Seconds; drain_policy = 'finish final in-flight request; elapsed includes drain';
            success = $true; p99_latency_ms = $p99 } |
            ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding UTF8
    }
    @{start_qpc_ticks=$startQpc.ToString(); end_qpc_ticks=[Diagnostics.Stopwatch]::GetTimestamp().ToString();
      qpc_frequency=[Diagnostics.Stopwatch]::Frequency; complete=$true; start_utc=$originUtc.ToString('o')} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'measurement.json') -Encoding UTF8
} catch {
    @{success=$false; error=$_.Exception.Message; actions=$count; skipped_arrivals=$missed} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $runDir 'summary.json') -Encoding UTF8
    throw
} finally {
    if ($null -ne $sampler) {
        if (!$sampler.HasExited) { Stop-Process -Id $sampler.Id -ErrorAction SilentlyContinue }
        $sampler.Dispose()
    }
    Write-Host "Results: $runDir"
}
