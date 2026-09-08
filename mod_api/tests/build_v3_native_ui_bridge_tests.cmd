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
if not exist "build\v3_native_ui_bridge_tests" (
    mkdir "build\v3_native_ui_bridge_tests"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude /Isrc ^
    tests\v3_native_ui_bridge_tests.cpp ^
    loader\v3_native_client_services.cpp ^
    /Fo"build\v3_native_ui_bridge_tests\\" ^
    /Fe:"build\v3_native_ui_bridge_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_native_ui_bridge_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo V3 native UI bridge regression passed.
popd
exit /b 0

:failed
echo V3 native UI bridge regression failed.
popd
exit /b 1
