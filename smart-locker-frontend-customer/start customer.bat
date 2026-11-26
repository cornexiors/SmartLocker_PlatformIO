@echo off
title Smart Locker Customer Frontend
cd /d "%~dp0"

if not exist "package.json" (
    echo package.json tidak ditemukan.
    pause
    exit /b 1
)

if not exist "node_modules" (
    echo Installing customer frontend dependencies...
    call npm install
    if errorlevel 1 (
        echo Gagal meng-install dependencies.
        pause
        exit /b 1
    )
)

echo Starting customer frontend...
start "" cmd /c "timeout /t 3 /nobreak >nul & start http://127.0.0.1:5174"

call npm run dev -- --host 127.0.0.1 --port 5174

echo.
echo Customer frontend stopped.
pause
