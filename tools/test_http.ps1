# Exercises every endpoint in docs/protocol.md against a live device and
# captures the serial log at the same time, so the HTTP response and the
# firmware's reaction can be compared side by side.
#
#   pwsh tools/test_http.ps1 -Ip 172.30.1.61 -Port COM6

param(
    [string]$Ip = "172.30.1.61",
    [string]$Port = "COM6",
    [int]$CaptureSeconds = 55
)

$ErrorActionPreference = "Continue"
$root = Split-Path -Parent $PSScriptRoot

$capture = Start-Job -ScriptBlock {
    param($script, $port, $seconds)
    & python $script $port $seconds
} -ArgumentList (Join-Path $PSScriptRoot "capture_serial.py"), $Port, $CaptureSeconds

function Invoke-Endpoint {
    param([string]$Name, [string]$Method, [string]$Path, [string]$Body)

    $uri = "http://$Ip$Path"
    try {
        $params = @{ Uri = $uri; Method = $Method; TimeoutSec = 5 }
        if ($Body) {
            $params.Body = $Body
            $params.ContentType = "application/json"
        }
        $response = Invoke-WebRequest @params
        "{0,-28} -> {1} {2}" -f $Name, $response.StatusCode, $response.Content
    }
    catch {
        $code = $_.Exception.Response.StatusCode.value__
        if ($code) {
            "{0,-28} -> {1} (expected for this case)" -f $Name, $code
        }
        else {
            "{0,-28} -> unreachable: {1}" -f $Name, $_.Exception.Message
        }
    }
}

# The capture resets the board, so wait for it to boot and join Wi-Fi.
# Invoke-WebRequest's own timeout paces the retries; no fixed sleep needed.
Write-Output "waiting for $Ip to answer /health ..."
$ready = $false
for ($i = 0; $i -lt 40; $i++) {
    try {
        Invoke-WebRequest -Uri "http://$Ip/health" -TimeoutSec 2 | Out-Null
        $ready = $true
        break
    }
    catch { }
}

if (-not $ready) {
    Write-Output "device never answered - is it on the network?"
    Receive-Job $capture -Wait
    exit 1
}

Write-Output "device answered"

# The capture job resets the board, and that reset can land in the middle of the
# HTTP run. Wait until /health reports a fresh boot before testing anything,
# otherwise the first cases talk to the old instance and the reboot shows up as
# a bogus timeout and bogus staleness.
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
    Write-Output "device never reported a fresh boot - aborting"
    Receive-Job $capture -Wait
    exit 1
}

Write-Output "fresh boot confirmed"

# The stale-alert check needs an absolute clock on the device, which arrives a
# few seconds after Wi-Fi via SNTP. Without it the firmware fails open and the
# test would silently prove nothing.
# /health answers instantly, so polling in a tight loop would burn through the
# retries in under a second and prove nothing. Pace it against the wall clock.
$clockSynced = $false
$deadline = (Get-Date).AddSeconds(30)
while ((Get-Date) -lt $deadline) {
    try {
        $health = Invoke-RestMethod -Uri "http://$Ip/health" -TimeoutSec 2
        if ($health.clock_synced) { $clockSynced = $true; break }
    }
    catch { }
    Start-Sleep -Milliseconds 1000
}

if ($clockSynced) {
    Write-Output "clock synced - stale check is meaningful`n"
}
else {
    Write-Output "WARNING: clock never synced, case 7 proves nothing`n"
}

$now = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$fresh = "test-$now"

Invoke-Endpoint "1. health"            "GET"  "/health"
Invoke-Endpoint "2. state BAD_PENDING" "POST" "/state" "{`"state`":`"BAD_PENDING`",`"score`":0.72,`"duration_ms`":3200}"
Invoke-Endpoint "3. state bogus (400)" "POST" "/state" "{`"state`":`"SLOUCHING`"}"
Invoke-Endpoint "4. malformed (400)"   "POST" "/state" "{not json"
Invoke-Endpoint "5. alert fresh"       "POST" "/alert" "{`"alert_id`":`"$fresh`",`"reason`":`"forward_head`",`"score`":0.86,`"timestamp`":$now,`"expires_in_ms`":5000}"
Invoke-Endpoint "6. alert duplicate"   "POST" "/alert" "{`"alert_id`":`"$fresh`",`"reason`":`"forward_head`",`"timestamp`":$now,`"expires_in_ms`":5000}"
Invoke-Endpoint "7. alert stale"       "POST" "/alert" "{`"alert_id`":`"stale-$now`",`"reason`":`"late`",`"timestamp`":$($now - 30000),`"expires_in_ms`":1000}"
Invoke-Endpoint "8. alert no id (400)" "POST" "/alert" "{`"reason`":`"oops`"}"
Invoke-Endpoint "9. mute 60s"          "POST" "/mute"  "{`"duration_ms`":60000}"
Invoke-Endpoint "10. alert while muted" "POST" "/alert" "{`"alert_id`":`"muted-$now`",`"timestamp`":$now,`"expires_in_ms`":5000}"
Invoke-Endpoint "11. mute cancel"      "POST" "/mute"  "{`"duration_ms`":0}"
Invoke-Endpoint "12. test alert"       "POST" "/test"
Invoke-Endpoint "13. unknown (404)"    "GET"  "/nope"
Invoke-Endpoint "14. health again"     "GET"  "/health"

Write-Output "`n=== SERIAL ==="
Receive-Job $capture -Wait
Remove-Job $capture
