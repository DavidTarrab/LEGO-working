param(
    [int]$Times = 1,
    [int]$MaxRetries = 100
)

$logDir = Join-Path -Path "." -ChildPath "logs\lego4_vs_nemotron4b_200APM"

if (-not (Test-Path -Path $logDir)) {
    New-Item -ItemType Directory -Path $logDir | Out-Null
}

# 获取当前最大日志编号
$existingLogs = Get-ChildItem -Path $logDir -Filter "log_*.txt" -File
$maxNumber = 0
foreach ($file in $existingLogs) {
    if ($file.Name -match 'log_(\d+)\.txt') {
        $num = [int]$matches[1]
        if ($num -gt $maxNumber) {
            $maxNumber = $num
        }
    }
}

$startNumber = $maxNumber + 1
$successfulRuns = 0
$totalAttempts = 0

while ($successfulRuns -lt $Times -and $totalAttempts -lt $MaxRetries) {
    $logIndex = $startNumber + $successfulRuns
    $logFile = Join-Path $logDir ("log_$logIndex.txt")

    $totalAttempts++
    Write-Host "Attempt #${totalAttempts}: Running 'make local', logging to $logFile"

    try {
        make local *>&1 | Out-File -FilePath $logFile -Encoding UTF8
    } catch {
        Write-Warning "Exception during run #${logIndex}: $($_.Exception.Message)"
        Start-Sleep -Seconds 1
        continue
    }

    # 读取日志内容，检查是否存在端口占用类错误
    $logContent = Get-Content -Path $logFile -Raw
    if ($logContent -match "ports are not available" -or
        $logContent -match "bind: An attempt was made to access a socket in a way forbidden by its access permissions" -or
        $logContent -match "Couldn't run: Error response from daemon: ports are not available" -or
        $logContent -match "Error response from daemon.*bind.*access.*forbidden") {
        
        Write-Warning "⚠️ Port binding error detected in log #$logIndex. Retrying this run..."
        Remove-Item $logFile -Force
        Start-Sleep -Seconds 2
        continue
    }

    $successfulRuns++
    Write-Host "✅ Run #$successfulRuns succeeded."
}

if ($successfulRuns -eq $Times) {
    Write-Host "`n🎉 All $Times successful runs completed. Logs are saved in '$logDir'"
} else {
    Write-Warning "`n⚠️ Only $successfulRuns successful runs completed after $totalAttempts attempts."
}
