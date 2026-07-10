# Direkter Build-Aufruf mit PYTHONUTF8=1
# Umgeht das Locale-Problem unter de_DE Windows

$ProjectPath = "C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\lora"
$IdfPath = "C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf"
$IdfPy = "$IdfPath\tools\idf.py"

Set-Location $ProjectPath

# ESP-IDF aktivieren
. ".\activate-esp-idf.ps1" | Out-Null

$env:PYTHONUTF8 = "1"
$env:LC_ALL = "en_US.UTF-8"
$env:IDF_PY_BUILD_JOBS = "6"

# Explizit python mit -X utf8 flag
Write-Host "Starte Build..." -ForegroundColor Cyan
$result = python -X utf8 $IdfPy build 2>&1
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "Build FEHLGESCHLAGEN (Exit: $exitCode)" -ForegroundColor Red
    $result | Select-Object -Last 30
    exit $exitCode
} else {
    Write-Host "Build ERFOLGREICH!" -ForegroundColor Green
}