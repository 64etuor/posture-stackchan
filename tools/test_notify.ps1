# Sends the notice types the PC monitor uses and captures the serial log.
# Korean bodies are sent as explicit UTF-8 bytes so encoding cannot be blamed
# if the screen shows garbage.
#
#   pwsh tools/test_notify.ps1 -Ip 172.30.1.61 -Port COM6

param(
    [string]$Ip = "172.30.1.61",
    [string]$Port = "COM6",
    [int]$CaptureSeconds = 60
)

$ErrorActionPreference = "Continue"

$capture = Start-Job -ScriptBlock {
    param($script, $port, $seconds)
    & python $script $port $seconds
} -ArgumentList (Join-Path $PSScriptRoot "capture_serial.py"), $Port, $CaptureSeconds

function Send-Notice {
    param([string]$Name, [string]$Json)

    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Json)
    try {
        $response = Invoke-WebRequest -Uri "http://$Ip/notify" -Method POST `
            -Body $bytes -ContentType "application/json; charset=utf-8" -TimeoutSec 5
        "{0,-12} -> {1} {2}" -f $Name, $response.StatusCode, $response.Content
    }
    catch {
        "{0,-12} -> failed: {1}" -f $Name, $_.Exception.Message
    }
}

Write-Output "waiting for a fresh boot on $Ip ..."
$booted = $false
$deadline = (Get-Date).AddSeconds(45)
while ((Get-Date) -lt $deadline) {
    try {
        $health = Invoke-RestMethod -Uri "http://$Ip/health" -TimeoutSec 2
        if ($health.uptime_ms -lt 20000) { $booted = $true; break }
    }
    catch { }
    Start-Sleep -Milliseconds 500
}

if (-not $booted) {
    Write-Output "no fresh boot seen - aborting"
    Receive-Job $capture -Wait
    exit 1
}

Write-Output "device is up`n"

Send-Notice "stretch" '{"text":"45분째 앉아 있어요\n일어나서 스트레칭!","kind":"stretch","duration_ms":9000}'
Start-Sleep -Seconds 11
Send-Notice "meal"    '{"text":"점심 시간이에요. 밥 먹고 오세요.","kind":"meal","duration_ms":9000}'
Start-Sleep -Seconds 11
Send-Notice "info"    '{"text":"자세 기준을 잡았어요","kind":"info","duration_ms":6000,"chime":false}'
Start-Sleep -Seconds 8
Send-Notice "no text" '{"kind":"info"}'

Write-Output "`n=== SERIAL ==="
Receive-Job $capture -Wait
Remove-Job $capture
