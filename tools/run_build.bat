@echo off
chcp 65001 > nul
cd /d %USERPROFILE%\Downloads\GitHub\VS-Projekte\CascadeProjects\lora
set PYTHONUTF8=1
set LC_ALL=C.UTF-8
set LANG=C.UTF-8
set IDF_PATH=%USERPROFILE%\Downloads\GitHub\VS-Projekte\CascadeProjects\esp-idf
call %IDF_PATH%\export.bat
set IDF_PY_BUILD_JOBS=6
echo Starting build...
idf.py build 2>&1
set EXIT_CODE=%ERRORLEVEL%
if %EXIT_CODE% NEQ 0 (
    echo ****************************************
    echo * Build failed with exit code %EXIT_CODE% *
    echo ****************************************
) else (
    echo ****************************************
    echo * Build SUCCESSFUL!                    *
    echo ****************************************
)
exit /b %EXIT_CODE%