@echo off
title Smart Locker Backend
cd /d "%~dp0"

rem Optional local-only secrets file:
rem   Create smart-locker-backend\local-secrets.bat with lines such as:
rem     set "BREVO_API_KEY=your-local-key"
rem     set "EMAIL_FROM=you@example.com"
rem     set "EMAIL_FROM_NAME=Smart Locker"
rem     set "MQTT_PROTOCOL=mqtts"
rem     set "MQTT_HOST=your-cluster-id.s1.eu.hivemq.cloud"
rem     set "MQTT_PORT=8883"
rem     set "MQTT_USERNAME=your-device-user"
rem     set "MQTT_PASSWORD=your-device-password"
rem     set "SMARTLOCKER_HMAC_SECRET=match-the-ESP32-secret"
rem   Keep this file untracked (add local-secrets.bat to .git/info/exclude or .gitignore).
rem   This launcher never writes the API key to disk.
set "_PRESET_BREVO_API_KEY=%BREVO_API_KEY%"
set "_PRESET_EMAIL_FROM=%EMAIL_FROM%"
set "_PRESET_EMAIL_FROM_NAME=%EMAIL_FROM_NAME%"
set "_PRESET_MQTT_PROTOCOL=%MQTT_PROTOCOL%"
set "_PRESET_MQTT_HOST=%MQTT_HOST%"
set "_PRESET_MQTT_PORT=%MQTT_PORT%"
set "_PRESET_MQTT_USERNAME=%MQTT_USERNAME%"
set "_PRESET_MQTT_PASSWORD=%MQTT_PASSWORD%"
set "_PRESET_SMARTLOCKER_HMAC_SECRET=%SMARTLOCKER_HMAC_SECRET%"
if exist "%~dp0local-secrets.bat" call "%~dp0local-secrets.bat"
if defined _PRESET_BREVO_API_KEY set "BREVO_API_KEY=%_PRESET_BREVO_API_KEY%"
if defined _PRESET_EMAIL_FROM set "EMAIL_FROM=%_PRESET_EMAIL_FROM%"
if defined _PRESET_EMAIL_FROM_NAME set "EMAIL_FROM_NAME=%_PRESET_EMAIL_FROM_NAME%"
if defined _PRESET_MQTT_PROTOCOL set "MQTT_PROTOCOL=%_PRESET_MQTT_PROTOCOL%"
if defined _PRESET_MQTT_HOST set "MQTT_HOST=%_PRESET_MQTT_HOST%"
if defined _PRESET_MQTT_PORT set "MQTT_PORT=%_PRESET_MQTT_PORT%"
if defined _PRESET_MQTT_USERNAME set "MQTT_USERNAME=%_PRESET_MQTT_USERNAME%"
if defined _PRESET_MQTT_PASSWORD set "MQTT_PASSWORD=%_PRESET_MQTT_PASSWORD%"
if defined _PRESET_SMARTLOCKER_HMAC_SECRET set "SMARTLOCKER_HMAC_SECRET=%_PRESET_SMARTLOCKER_HMAC_SECRET%"
set "_PRESET_BREVO_API_KEY="
set "_PRESET_EMAIL_FROM="
set "_PRESET_EMAIL_FROM_NAME="
set "_PRESET_MQTT_PROTOCOL="
set "_PRESET_MQTT_HOST="
set "_PRESET_MQTT_PORT="
set "_PRESET_MQTT_USERNAME="
set "_PRESET_MQTT_PASSWORD="
set "_PRESET_SMARTLOCKER_HMAC_SECRET="

if not defined BREVO_API_KEY (
    echo BREVO_API_KEY is not set.
    echo Standard batch input is visible while typing. For safer entry, use the
    echo untracked local-secrets.bat option described above.
    set /p "BREVO_API_KEY=Enter BREVO_API_KEY (leave blank to disable Brevo OTP): "
)

if not defined EMAIL_FROM set "EMAIL_FROM=lokerkulokerkita@gmail.com"

if not defined EMAIL_FROM_NAME set "EMAIL_FROM_NAME=Smart Locker"

if not exist "node_modules" (
    echo Installing dependencies...
    call npm install
    if errorlevel 1 (
        echo Failed to install backend dependencies.
        pause
        exit /b 1
    )
)

set CORS_ORIGINS=http://localhost:5173,http://127.0.0.1:5173,http://localhost:5174,http://127.0.0.1:5174

echo Starting Node.js backend and initializing the SQLite database...
call npm start

echo.
echo Backend stopped.
pause