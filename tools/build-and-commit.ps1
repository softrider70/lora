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

# Build-Nummer inkrementieren
$incrementScript = Join-Path $ProjectPath "tools\increment_build.py"
if (Test-Path $incrementScript) {
    Write-Host "Incrementing build number..." -ForegroundColor Cyan
    & python "$incrementScript"
}

# Build-Nummer auslesen
$buildNumberPath = Join-Path $ProjectPath ".build_number"
$buildNumber = if (Test-Path $buildNumberPath) { Get-Content $buildNumberPath -Raw } else { "?" }

# Build ausführen
Write-Host "Building project..." -ForegroundColor Cyan
$activateScript = Join-Path $ProjectPath "activate-esp-idf.ps1"
$buildCmd = ". '$activateScript'; `$env:IDF_PY_BUILD_JOBS = '6'; idf.py build"
$buildStartTime = Get-Date
powershell -ExecutionPolicy Bypass -NoProfile -Command $buildCmd | Out-Null
$buildExitCode = $LASTEXITCODE
$buildDuration = ((Get-Date) - $buildStartTime).TotalSeconds

if ($buildExitCode -ne 0) {
    Write-Host "Build failed! (Dauer: $($buildDuration.ToString('F1'))s)" -ForegroundColor Red
    exit $buildExitCode
}

Write-Host "Build erfolgreich! (Dauer: $($buildDuration.ToString('F1'))s)" -ForegroundColor Green

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