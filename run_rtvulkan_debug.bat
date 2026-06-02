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
set "RTV_EXIT_CODE=%ERRORLEVEL%"

if not "%RTV_EXIT_CODE%"=="0" (
    echo.
    echo rtvulkan.exe exited with code %RTV_EXIT_CODE%.
    echo The engine may have crashed. Press any key to close this terminal.
    pause >nul
)

endlocal & exit /b %RTV_EXIT_CODE%
