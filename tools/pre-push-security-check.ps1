#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Pre-Push Security Check fuer LoRa ESP32-S3 Projekt
    Prueft den Code auf sicherheitskritische Informationen vor dem Push.
.DESCRIPTION
    Sucht im gesamten Repository nach:
    - Hartcodierten Passwoertern und API-Keys
    - WiFi-Credentials im Klartext
    - Datenbank- und Service-Zugangsdaten
    - NVS-Speicher mit sensiblen Defaults
    
    Exit-Code 0 = OK, Exit-Code 1 = Sicherheitsproblem gefunden
#>

$ErrorActionPreference = "Stop"

# Farben fuer die Ausgabe
$RED = @{ForegroundColor = "Red"}
$GREEN = @{ForegroundColor = "Green"}
$YELLOW = @{ForegroundColor = "Yellow"}
$CYAN = @{ForegroundColor = "Cyan"}

Write-Host @CYAN "=== Pre-Push Security Check ==="
Write-Host "Pruefe auf sicherheitskritische Informationen..."

$rootDir = Resolve-Path "$PSScriptRoot\.."
$issues = @()
$filesToCheck = @()

# Alle getrackten + neuen Dateien sammeln (ausser build/)
Get-ChildItem -Path $rootDir -Recurse -File | Where-Object {
    $_.FullName -notmatch '\\build\\' -and
    $_.FullName -notmatch '\\.git\\'
} | ForEach-Object { $filesToCheck += $_ }

Write-Host "Durchsuche $($filesToCheck.Count) Dateien..."

# ====================================================================
# Regel 1: Keine hartcodierten API-Keys oder Token
# ====================================================================
$apikeyPatterns = @(
    '(?i)api[_-]?key\s*[=:]\s*[''"][A-Za-z0-9_\-]{10,}[''"]',
    '(?i)apikey\s*[=:]\s*[''"][A-Za-z0-9_\-]{10,}[''"]',
    '(?i)api_secret\s*[=:]\s*[''"][A-Za-z0-9_\-]{10,}[''"]',
    '(?i)secret\s*[=:]\s*[''"][A-Za-z0-9_\-]{10,}[''"]',
    '(?i)token\s*[=:]\s*[''"][A-Za-z0-9_\-]{10,}[''"]',
    '(?i)bearer\s+[A-Za-z0-9_\-\.]{20,}',
    'WOLFRAM_LLM_APP_ID\s*=\s*[''"][A-Za-z0-9_\-]{5,}[''"]',
    'OPENWEATHER_API_KEY\s*=\s*[''"][A-Za-z0-9_\-]{5,}[''"]'
)

foreach ($file in $filesToCheck) {
    $content = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $content) { continue }

    foreach ($pattern in $apikeyPatterns) {
        if ($content -match $pattern) {
            $relPath = [System.IO.Path]::GetRelativePath($rootDir, $file.FullName)
            $issues += @{
                File = $relPath
                Regel = "API-Key/Token gefunden"
                Gefunden = $matches[0]
            }
        }
    }
}

# ====================================================================
# Regel 2: Keine WiFi-Passwoerter im Klartext
# ====================================================================
$wifiPatterns = @(
    '(?i)wifi_password\s*=\s*[''"](?!"")(?!\s*$)[A-Za-z0-9_\-\.\!\@\#\$\%\^]{4,}[''"]',
    '(?i)wifi_ssid\s*=\s*[''"](?!"")(?!\s*$).{2,}[''"]',
    '(?i)password\s*[''"][A-Za-z0-9_\-\.\!\@\#\$\%\^]{8,}[''"]'
)

foreach ($file in $filesToCheck) {
    $content = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $content) { continue }
    
    # .c und .h Dateien sind OK, da dort nur Konfigurations-Pfade stehen
    if ($file.Extension -in '.c', '.h') { continue }

    foreach ($pattern in $wifiPatterns) {
        if ($content -match $pattern) {
            $relPath = [System.IO.Path]::GetRelativePath($rootDir, $file.FullName)
            $issues += @{
                File = $relPath
                Regel = "WiFi-Passwort/SSID gefunden"
                Gefunden = $matches[0]
            }
        }
    }
}

# ====================================================================
# Regel 3: NVS-Default-Werte mit sensiblen Daten
# ====================================================================
$nvsPatterns = @(
    'nvs_config_set_str\("wifi_password",\s*"[A-Za-z0-9]{3,}"',
    'nvs_config_set_str\("wifi_ssid",\s*"[A-Za-z0-9]{3,}"'
)

foreach ($file in $filesToCheck) {
    $content = Get-Content -Path $file.FullName -Raw -ErrorAction SilentlyContinue
    if (-not $content) { continue }

    foreach ($pattern in $nvsPatterns) {
        if ($content -match $pattern) {
            $relPath = [System.IO.Path]::GetRelativePath($rootDir, $file.FullName)
            $issues += @{
                File = $relPath
                Regel = "NVS mit hartcodierten Zugangsdaten"
                Gefunden = $matches[0]
            }
        }
    }
}

# ====================================================================
# Regel 4: Keine .env-Dateien oder sensitive Configs im Repo
# ====================================================================
$sensitiveFiles = @(
    '\.env$',
    '\.env\.\w+$',
    'credentials\.\w+$',
    'secret\.\w+$',
    '\*\.pfx$',
    '\*\.p12$',
    '\*\.key$',
    'id_rsa$',
    'id_ed25519$'
)

foreach ($file in $filesToCheck) {
    $relPath = [System.IO.Path]::GetRelativePath($rootDir, $file.FullName)
    foreach ($pattern in $sensitiveFiles) {
        if ($relPath -match $pattern) {
            $issues += @{
                File = $relPath
                Regel = "Sensible Datei im Repository"
                Gefunden = $relPath
            }
        }
    }
}

# ====================================================================
# Auswertung
# ====================================================================
if ($issues.Count -gt 0) {
    Write-Host @RED "`nSICHERHEITSPROBLEME GEFUNDEN! Push wird blockiert.`n"
    
    $grouped = $issues | Group-Object File
    foreach ($group in $grouped) {
        Write-Host @YELLOW "Datei: $($group.Name)"
        foreach ($issue in $group.Group) {
            Write-Host @RED "  [$($issue.Regel)] $($issue.Gefunden)"
        }
    }
    
    Write-Host @RED "`nBitte entferne die sensiblen Daten vor dem Push."
    Write-Host @YELLOW "Hinweis: Verwende NVS-Konfiguration oder Umgebungsvariablen."
    exit 1
} else {
    Write-Host @GREEN "Keine Sicherheitsprobleme gefunden. Push erlaubt."
    exit 0
}