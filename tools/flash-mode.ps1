param(
    [ValidateSet('usb','ota','last','beide')]
    [string]$Mode = 'last',
    [string]$UsbPort = 'COM3',          # Sensor-Node mit GPS
    [string]$UsbPort2 = 'COM8',         # Anzeige-Node ohne GPS
    [string]$OtaUrl = 'http://lora-node.local:8080/update'
)

$ProjectPath = Resolve-Path -Path "$PSScriptRoot\.."
Set-Location $ProjectPath

$lastFile = ".last_flash_mode.json"

if ($Mode -eq 'last' -and (Test-Path $lastFile)) {
    $last = Get-Content $lastFile | ConvertFrom-Json
    $Mode = $last.mode
    $UsbPort = $last.port
}

Write-Host "Flash mode: $Mode" -ForegroundColor Cyan

function Stop-OrphanMonitor {
    # Verwaiste Monitor-Prozesse belegen den COM-Port und muessen weg.
    $orphans = Get-CimInstance Win32_Process |
        Where-Object { $_.CommandLine -like '*idf_monitor*' }
    foreach ($p in $orphans) {
        Write-Host "Beende verwaisten Monitor (PID $($p.ProcessId))" -ForegroundColor Yellow
        Stop-Process -Id $p.ProcessId -Force -ErrorAction SilentlyContinue
    }
}

function Test-ComPort([string]$Port) {
    # Nur Ports mit Status OK sind wirklich angeschlossen. Ein abgezogenes
    # Board bleibt als Geraet mit Status "Unknown" stehen - der Flashversuch
    # scheitert dann mit "Could not open COMx".
    $found = Get-PnpDevice -Class Ports -Status OK -ErrorAction SilentlyContinue |
        Where-Object { $_.FriendlyName -like "*($Port)*" }
    if (-not $found) {
        Write-Host "Port $Port ist nicht angeschlossen - Kabel pruefen!" -ForegroundColor Red
        return $false
    }
    return $true
}

function Flash-UsbPort([string]$Port) {
    if (-not (Test-ComPort $Port)) { return $false }

    Stop-OrphanMonitor

    $activateScript = Join-Path $ProjectPath "activate-esp-idf.ps1"
    # exit $LASTEXITCODE weiterreichen, sonst sieht ein Fehlschlag wie Erfolg aus
    $cmd = ". '$activateScript'; idf.py -p $Port flash; exit `$LASTEXITCODE"
    powershell -ExecutionPolicy Bypass -NoProfile -Command $cmd

    if ($LASTEXITCODE -eq 0) {
        Write-Host "Flash auf $Port erfolgreich" -ForegroundColor Green
        return $true
    }

    Write-Host "Flash auf $Port fehlgeschlagen (Code $LASTEXITCODE)" -ForegroundColor Red
    return $false
}

if ($Mode -eq 'usb') {
    if (Flash-UsbPort $UsbPort) {
        @{mode='usb'; port=$UsbPort} | ConvertTo-Json | Set-Content $lastFile
        Write-Host "USB Flash erfolgreich" -ForegroundColor Green
    }
} elseif ($Mode -eq 'beide') {
    # Sensor-Node und Anzeige-Node nacheinander flashen
    $ok1 = Flash-UsbPort $UsbPort
    $ok2 = Flash-UsbPort $UsbPort2
    if ($ok1 -and $ok2) {
        @{mode='usb'; port=$UsbPort} | ConvertTo-Json | Set-Content $lastFile
    } else {
        Write-Host "Mindestens ein Board wurde nicht geflasht" -ForegroundColor Red
    }
} elseif ($Mode -eq 'ota') {
    $binPath = "build\lora.bin"
    if (Test-Path $binPath) {
        Write-Host "Uploading $binPath to $OtaUrl ..." -ForegroundColor Cyan
        $form = @{firmware = Get-Item -Path $binPath}
        Invoke-RestMethod -Uri $OtaUrl -Method Post -Form $form -TimeoutSec 60
        Write-Host "OTA Update gesendet" -ForegroundColor Green
    } else {
        Write-Host "Binary not found: $binPath" -ForegroundColor Red
    }
}