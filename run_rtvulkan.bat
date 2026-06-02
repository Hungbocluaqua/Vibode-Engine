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
set "RTV_EXIT_CODE=%ERRORLEVEL%"

if not "%RTV_EXIT_CODE%"=="0" (
    echo.
    echo rtvulkan.exe exited with code %RTV_EXIT_CODE%.
    echo The engine may have crashed. Press any key to close this terminal.
    pause >nul
)

endlocal & exit /b %RTV_EXIT_CODE%
