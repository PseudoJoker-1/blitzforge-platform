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
if not "%errorlevel%"=="0" (
    echo vcvars32 failed.
    popd
    exit /b 1
)

if not exist "build" mkdir "build"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_dava_native_registry_tests.cpp ^
    src\v3\dava_native_registry.cpp ^
    /Fo"build\\" ^
    /link /OUT:"build\v3_dava_native_registry_tests.exe" ^
    /IMPLIB:"build\v3_dava_native_registry_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_dava_native_registry_tests.exe"
if not "%errorlevel%"=="0" goto :failed

popd
exit /b 0

:failed
echo V3 DAVA native registry tests failed.
popd
exit /b 1
