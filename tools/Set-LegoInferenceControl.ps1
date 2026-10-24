[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Name,
    [Parameter(Mandatory)][ValidateSet('Pause', 'Resume', 'Stop')][string]$Action,
    [ValidateRange(0, 60000)][int]$WaitPausedMs = 0
)
$ErrorActionPreference = 'Stop'
$event = $null
$acknowledgement = $null
try {
    $event = [Threading.EventWaitHandle]::OpenExisting("Local\LEGO-$Name-$Action")
    $null = $event.Set()
    if ($Action -eq 'Pause' -and $WaitPausedMs -gt 0) {
        $acknowledgement = [Threading.EventWaitHandle]::OpenExisting("Local\LEGO-$Name-Paused")
        if (!$acknowledgement.WaitOne($WaitPausedMs)) { throw 'Pause requested, but no checkpoint acknowledged it before timeout. Resume or stop explicitly if needed.' }
        Write-Host 'Inference is waiting at a completed graph checkpoint.'
    } else { Write-Host "$Action requested for $Name." }
} finally {
    if ($acknowledgement) { $acknowledgement.Dispose() }
    if ($event) { $event.Dispose() }
}
