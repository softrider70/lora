@echo off
chcp 65001 > nul
set PYTHONUTF8=1
set IDF_PATH=C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf
cd /d C:\Users\user\Downloads\GitHub\VS-Projekte\CascadeProjects\lora
call %IDF_PATH%\export.bat > nul
set IDF_PY_BUILD_JOBS=6
idf.py build
if %ERRORLEVEL% NEQ 0 (
    echo Build FEHLGESCHLAGEN!
    pause
    exit /b 1
)
echo Build erfolgreich!
pause