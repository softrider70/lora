param(
    [ValidateSet('usb','ota','last')]
    [string]$Mode = 'last',
    [string]$UsbPort = 'COM11',
    [string]$OtaUrl = 'http://lora-node.local/update'
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

if ($Mode -eq 'usb') {
    # ESP-IDF Umgebung aktivieren und flashen
    $activateScript = Join-Path $ProjectPath "activate-esp-idf.ps1"
    $cmd = ". '$activateScript'; idf.py -p $UsbPort flash"
    powershell -ExecutionPolicy Bypass -NoProfile -Command $cmd
    if ($LASTEXITCODE -eq 0) {
        @{mode='usb'; port=$UsbPort} | ConvertTo-Json | Set-Content $lastFile
        Write-Host "USB Flash erfolgreich" -ForegroundColor Green
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