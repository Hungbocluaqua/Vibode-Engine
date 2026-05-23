@echo off
setlocal

cd /d "%~dp0"

if not exist "build\Debug\rtvulkan.exe" (
    echo Debug executable not found.
    echo Build it first with: cmake --build build --config Debug
    pause
    exit /b 1
)

"build\Debug\rtvulkan.exe" %*

endlocal
