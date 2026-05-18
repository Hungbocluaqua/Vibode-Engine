@echo off
setlocal

cd /d "%~dp0"

if not exist "build\Release\rtvulkan.exe" (
    echo Release executable not found.
    echo Build it first with: cmake --build build --config Release
    pause
    exit /b 1
)

"build\Release\rtvulkan.exe" %*

endlocal
