@echo off
setlocal

cd /d "%~dp0"

set "RTV_EXE="
for %%P in ("build\Release\rtvulkan.exe" "build-codex\Release\rtvulkan.exe" "build-rtxdi-clean\Release\rtvulkan.exe" "build-rtxdi-test\Release\rtvulkan.exe") do (
    if not defined RTV_EXE if exist "%%~P" set "RTV_EXE=%%~P"
)

if not defined RTV_EXE (
    echo Release executable not found in build, build-codex, or RTXDI build directories.
    echo Build it first with: cmake --build build --config Release
    pause
    exit /b 1
)

"%RTV_EXE%" %*
set "RTV_EXIT_CODE=%ERRORLEVEL%"

if not "%RTV_EXIT_CODE%"=="0" (
    echo.
    echo rtvulkan.exe exited with code %RTV_EXIT_CODE%.
    echo The engine may have crashed. Press any key to close this terminal.
    pause >nul
)

endlocal & exit /b %RTV_EXIT_CODE%
