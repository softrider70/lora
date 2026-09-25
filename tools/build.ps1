# Build-Skript fuer LoRa ESP32-S3 Projekt
# Umgeht das Locale-Problem unter Windows

$ProjectPath = "$env:USERPROFILE\Downloads\GitHub\VS-Projekte\CascadeProjects\lora"
$IdfPath = "$env:USERPROFILE\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf"

Set-Location $ProjectPath

# ESP-IDF aktivieren
. ".\activate-esp-idf.ps1" | Out-Null

# Locale und Python-Flags setzen
$env:PYTHONUTF8 = "1"
$env:IDF_PY_BUILD_JOBS = "6"

# Python-Sitecustomize schreiben, um locale zu fixen
$sitePackages = python -c "import site; print(site.getusersitepackages())" 2>&1
if (-not (Test-Path $sitePackages)) {
    New-Item -ItemType Directory -Path $sitePackages -Force | Out-Null
}
$sitecustomize = @"
import locale
try:
    locale.setlocale(locale.LC_ALL, '.UTF-8')
except:
    try:
        locale.setlocale(locale.LC_ALL, '')
    except:
        pass
"@
$sitecustomize | Out-File -FilePath (Join-Path $sitePackages "sitecustomize.py") -Encoding utf8

Write-Host "Starte Build..." -ForegroundColor Cyan
$buildResult = idf.py build 2>&1
$exitCode = $LASTEXITCODE

if ($exitCode -ne 0) {
    Write-Host "Build FEHLGESCHLAGEN (Exit: $exitCode)" -ForegroundColor Red
    Write-Host $buildResult
    exit $exitCode
} else {
    Write-Host "Build ERFOLGREICH!" -ForegroundColor Green
}