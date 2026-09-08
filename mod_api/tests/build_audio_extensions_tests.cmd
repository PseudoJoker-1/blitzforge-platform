@echo off
setlocal
pushd "%~dp0\.."

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo Visual Studio 2022 C++ tools were not found.
    goto :failed
)

call "%VCVARS%" >nul
if not "%errorlevel%"=="0" goto :failed
if not exist "build" mkdir "build"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\audio_extensions_tests.cpp ^
    src\wotb_mod_windows_audio.cpp ^
    src\wotb_mod_dava_sound.cpp ^
    loader\v3_native_client_services.cpp ^
    xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib ^
    /Fo"build\\" ^
    /link /OUT:"build\audio_extensions_tests.exe" ^
    /IMPLIB:"build\audio_extensions_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\audio_extensions_tests.exe"
if not "%errorlevel%"=="0" goto :failed

popd
exit /b 0

:failed
echo Audio extension tests failed.
popd
exit /b 1
