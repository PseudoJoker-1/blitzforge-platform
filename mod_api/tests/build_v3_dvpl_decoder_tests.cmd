@echo off
setlocal
pushd "%~dp0\.."

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo Visual Studio 2022 C++ tools were not found.
    popd
    exit /b 1
)

call "%VCVARS%" >nul
if not "%errorlevel%"=="0" goto :failed

if not exist "build" mkdir "build"
if not exist "build\v3_dvpl_decoder_tests" (
    mkdir "build\v3_dvpl_decoder_tests"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude /Isrc ^
    tests\v3_dvpl_decoder_tests.cpp ^
    src\v3\dvpl_decoder.cpp ^
    /Fo"build\v3_dvpl_decoder_tests\\" ^
    /Fe:"build\v3_dvpl_decoder_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_dvpl_decoder_tests.exe" %*
if not "%errorlevel%"=="0" goto :failed

echo V3 DVPL decoder regression passed.
popd
exit /b 0

:failed
echo V3 DVPL decoder regression failed.
popd
exit /b 1
