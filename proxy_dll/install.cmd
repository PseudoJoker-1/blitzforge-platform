@echo off
REM Install proxy DLL into the game directory.
REM This:
REM   1. Copies the system version.dll to vorig.dll (next to wotblitz.exe)
REM   2. Copies our built version.dll to the game folder
REM   3. Copies the public native runtime loaded by the proxy
REM   4. Backs up the existing version.dll (none expected, but just in case)

setlocal
set GAME_DIR=%~dp0..\..
set SYS_VERSION=%SystemRoot%\SysWOW64\version.dll
set MOD_LOADER=%~dp0..\mod_api\build\wotb_mod_loader.dll

if not exist "%~dp0version.dll" ( echo Build first: build.cmd & exit /b 1 )
if not exist "%MOD_LOADER%" ( echo Build native loader first: ..\mod_api\loader\build_live.cmd & exit /b 1 )

if exist "%GAME_DIR%\version.dll" (
    if not exist "%GAME_DIR%\version.dll.bak" (
        copy /y "%GAME_DIR%\version.dll" "%GAME_DIR%\version.dll.bak" >nul
        echo Backed up existing version.dll -> version.dll.bak
    )
)

if not exist "%GAME_DIR%\vorig.dll" (
    copy /y "%SYS_VERSION%" "%GAME_DIR%\vorig.dll" >nul || ( echo failed to copy system version.dll & exit /b 1 )
) else (
    echo Keeping existing vorig.dll
)
copy /y "%~dp0version.dll" "%GAME_DIR%\version.dll" >nul || ( echo failed to copy proxy & exit /b 1 )
copy /y "%MOD_LOADER%" "%GAME_DIR%\wotb_mod_loader.dll" >nul || ( echo failed to copy native loader & exit /b 1 )

echo Installed:
echo   "%GAME_DIR%\version.dll" (proxy)
echo   "%GAME_DIR%\vorig.dll"   (real version.dll)
echo   "%GAME_DIR%\wotb_mod_loader.dll" (native runtime)
echo.
echo Run the game; check "%GAME_DIR%\wotb_mod.log" for proof of injection.
endlocal
