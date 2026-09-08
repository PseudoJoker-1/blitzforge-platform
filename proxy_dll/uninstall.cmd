@echo off
REM Uninstall proxy: restore original / remove our files.
setlocal
set GAME_DIR=%~dp0..\..

if exist "%GAME_DIR%\version.dll" del /q "%GAME_DIR%\version.dll"
if exist "%GAME_DIR%\vorig.dll"   del /q "%GAME_DIR%\vorig.dll"
if exist "%GAME_DIR%\wotb_mod_loader.dll" del /q "%GAME_DIR%\wotb_mod_loader.dll"
if exist "%GAME_DIR%\version.dll.bak" (
    move /y "%GAME_DIR%\version.dll.bak" "%GAME_DIR%\version.dll" >nul
    echo Restored original version.dll from backup.
) else (
    echo Removed proxy. No backup existed (game uses system version.dll normally).
)
endlocal
