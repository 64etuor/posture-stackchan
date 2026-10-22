# Registers a Task Scheduler task that starts the PC posture monitor at logon.
#
#   powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1            # install / update
#   powershell -ExecutionPolicy Bypass -File tools\install_autostart.ps1 -Uninstall # remove
#
# Runs as the logged-on user (the webcam and the voice need the interactive
# session) through pythonw, so no console window appears. Log: pc\logs\monitor.log

param([switch]$Uninstall)

$TaskName = "posture-stackchan monitor"

if ($Uninstall) {
    Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false
    Write-Host "[autostart] removed task '$TaskName'"
    return
}

$pcDir = Join-Path (Split-Path $PSScriptRoot -Parent) "pc"
$pythonw = (Get-Command pythonw.exe -ErrorAction Stop).Source
# Skip the WindowsApps alias stub; it is not a real interpreter.
if ($pythonw -like "*\WindowsApps\*") {
    $pythonw = Join-Path (Split-Path (Get-Command python.exe).Source) "pythonw.exe"
}

$action = New-ScheduledTaskAction -Execute $pythonw -Argument "autostart.pyw" -WorkingDirectory $pcDir
$trigger = New-ScheduledTaskTrigger -AtLogOn -User "$env:USERDOMAIN\$env:USERNAME"
# Give Wi-Fi and the camera driver a moment after logon.
$trigger.Delay = "PT30S"
$settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries `
    -ExecutionTimeLimit ([TimeSpan]::Zero) -MultipleInstances IgnoreNew
$principal = New-ScheduledTaskPrincipal -UserId "$env:USERDOMAIN\$env:USERNAME" -LogonType Interactive -RunLevel Limited

Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger `
    -Settings $settings -Principal $principal -Force | Out-Null

Write-Host "[autostart] registered '$TaskName' -> $pythonw autostart.pyw (in $pcDir)"
