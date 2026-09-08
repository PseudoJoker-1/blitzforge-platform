@echo off
setlocal
rem WotbMod portal, local run: first start creates config.json, the data folder,
rem the signing key and the administrator, then opens the site in the browser.
pushd "%~dp0"

where python >nul 2>nul || (echo Python 3.11+ is required. Install it from python.org and re-run. & popd & exit /b 1)

if not exist "config.json" (
    copy /y "config.example.json" "config.json" >nul
    echo Created config.json from config.example.json. Change admin_password before hosting.
)

if not exist "data" mkdir "data"
if not exist "data\portal.key" (
    echo Creating the portal signing key...
    python "tools\portal_keygen.py" --out "data\portal.key" --key-id blitzforge-portal-2026 || (popd & exit /b 1)
)

python "backend\portal_server.py" init || (popd & exit /b 1)

if "%1"=="seed" (
    python "seed_demo.py" || (popd & exit /b 1)
)

echo.
echo Starting the portal. Press Ctrl+C to stop.
python "backend\portal_server.py" serve --open
popd
