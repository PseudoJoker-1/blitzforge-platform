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

if not exist "build\v3_client_truthful_tests" (
    mkdir "build\v3_client_truthful_tests"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX /c ^
    /Iinclude ^
    src\v3\client_services.cpp ^
    /Fo"build\v3_client_truthful_tests\client_services.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX /c ^
    /Iinclude ^
    src\v3\runtime_services.cpp ^
    /Fo"build\v3_client_truthful_tests\runtime_services.obj"
if not "%errorlevel%"=="0" goto :failed

lib /nologo ^
    /OUT:"build\v3_client_truthful_tests\runtime_without_client.lib" ^
    "build\wotb_mod_runtime.lib" ^
    /REMOVE:"build\v3\client_services.obj" ^
    /REMOVE:"build\v3\runtime_services.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_client_truthful_slices_tests.cpp ^
    "build\v3_client_truthful_tests\client_services.obj" ^
    "build\v3_client_truthful_tests\runtime_services.obj" ^
    "build\v3_client_truthful_tests\runtime_without_client.lib" ^
    /Fo"build\v3_client_truthful_tests\test.obj" ^
    /link /OUT:"build\v3_client_truthful_tests\test.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_client_truthful_tests\test.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 truthful client slices tests passed ===
popd
exit /b 0

:failed
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%
