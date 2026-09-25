param(
    [string]$ProjectPath = "$PSScriptRoot\.."
)

$ProjectPath = Resolve-Path -Path $ProjectPath
Set-Location $ProjectPath

# ESP-IDF Umgebung aktivieren
$espIdfPath = "C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf"
$exportBat = Join-Path $espIdfPath "export.bat"

if (-not (Test-Path $exportBat)) {
    Write-Host "ESP-IDF export.bat nicht gefunden: $exportBat" -ForegroundColor Red
    exit 1
}

# Build ausfuehren. Die Build-Nummer erhoeht das CMake-Ziel lora_version
# waehrend des Builds (tools/increment_build.py) - deshalb hier kein Aufruf.
Write-Host "Building project..." -ForegroundColor Cyan
$activateScript = Join-Path $ProjectPath "activate-esp-idf.ps1"
$buildCmd = ". '$activateScript'; `$env:IDF_PY_BUILD_JOBS = '6'; idf.py build; exit `$LASTEXITCODE"

$buildLog = Join-Path $ProjectPath "build\build.log"
$buildStartTime = Get-Date
# Ausgabe mitschreiben und anzeigen. Wird sie verschluckt, fehlt bei einem
# Fehlschlag die Ursache.
powershell -ExecutionPolicy Bypass -NoProfile -Command $buildCmd 2>&1 |
    Tee-Object -FilePath $buildLog | Select-Object -Last 25
$buildExitCode = $LASTEXITCODE
$buildDuration = ((Get-Date) - $buildStartTime).TotalSeconds

# Build-Nummer auslesen (schreibt der Build selbst)
$buildNumberPath = Join-Path $ProjectPath ".build_number"
$buildNumber = if (Test-Path $buildNumberPath) { Get-Content $buildNumberPath -Raw } else { "?" }

if ($buildExitCode -ne 0) {
    Write-Host "Build fehlgeschlagen! (Dauer: $($buildDuration.ToString('F1'))s)" -ForegroundColor Red
    Write-Host "Log: $buildLog" -ForegroundColor Yellow
    exit $buildExitCode
}

Write-Host "Build erfolgreich! (Dauer: $($buildDuration.ToString('F1'))s, Build $buildNumber)" -ForegroundColor Green

# Nur Metadaten committen (nicht Build-Artefakte)
Write-Host "Staging Metadaten..." -ForegroundColor Cyan
& git add -f .build_number
& git add -f include/version.h 2>$null

$commitMessage = "chore: build #$buildNumber"
& git commit -m $commitMessage 2>&1

if ($LASTEXITCODE -eq 0) {
    Write-Host "Metadaten committed: $commitMessage" -ForegroundColor Green
    Write-Host "Pushing to remote..." -ForegroundColor Cyan
    $pushResult = & git push 2>&1
    if ($LASTEXITCODE -eq 0) {
        Write-Host "Push successful" -ForegroundColor Green
    } else {
        Write-Host "Push failed (manual push needed): $pushResult" -ForegroundColor Yellow
    }
} else {
    Write-Host "No changes to commit" -ForegroundColor Yellow
}

Write-Host "Build #$buildNumber completed" -ForegroundColor Green