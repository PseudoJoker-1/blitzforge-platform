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
if not exist "build\v3_post_rc1_services" (
    mkdir "build\v3_post_rc1_services"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_post_rc1_services_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_post_rc1_services\\" ^
    /Fe:"build\v3_post_rc1_services\test.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_post_rc1_services\test.exe" "build\v3_post_rc1_services\env"
if not "%errorlevel%"=="0" goto :failed

echo === V3 post-RC1 declared services tests passed ===
popd
exit /b 0

:failed
set "STATUS=%ERRORLEVEL%"
echo V3 post-RC1 declared services tests failed.
popd
exit /b %STATUS%
