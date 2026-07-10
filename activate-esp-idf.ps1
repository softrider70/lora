# Aktiviert die vorhandene ESP-IDF Installation fuer diese PowerShell-Session.
# Verwendung: . .\activate-esp-idf.ps1
# Workaround fuer Python 3.14 + deutsches Locale: PYTHONUTF8 auf 0 setzen

param(
    [string]$IdfPath = $env:IDF_PATH,
    [string]$IdfToolsPath = $env:IDF_TOOLS_PATH
)

$env:PYTHONUTF8 = "0"

$idfCandidates = @(
    $IdfPath,
    "C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf",
    "C:\esp\v6.1\esp-idf",
    "C:\esp\esp-idf"
) | Where-Object { $_ -and (Test-Path $_) }

$resolvedIdfPath = $idfCandidates | Select-Object -First 1

if (-not $resolvedIdfPath) {
    Write-Error "ESP-IDF wurde nicht gefunden. Setze IDF_PATH oder uebergib -IdfPath."
    return
}

$env:IDF_PATH = $resolvedIdfPath
if ($IdfToolsPath) {
    $env:IDF_TOOLS_PATH = $IdfToolsPath
}

# export.ps1 aus PowerShell heraus direkt ausfuehren
$exportScript = Join-Path $resolvedIdfPath "export.ps1"
if (Test-Path $exportScript) {
    & $exportScript | Out-Host
    Write-Host "ESP-IDF Umgebung aktiviert." -ForegroundColor Green
} else {
    # Fallback: export.bat parsen
    $exportBat = Join-Path $resolvedIdfPath "export.bat"
    if (Test-Path $exportBat) {
        cmd /c "set PYTHONUTF8=0 && ""$exportBat"" > nul && set" | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)') {
                Set-Item -Path "env:$($matches[1])" -Value $matches[2]
            }
        }
        Write-Host "ESP-IDF Umgebung aktiviert (via export.bat)." -ForegroundColor Green
    } else {
        Write-Error "Weder export.ps1 noch export.bat gefunden unter: $resolvedIdfPath"
        return
    }
}

Write-Host "IDF_PATH: $env:IDF_PATH" -ForegroundColor Cyan
Write-Host "IDF_PYTHON_ENV_PATH: $env:IDF_PYTHON_ENV_PATH" -ForegroundColor Cyan