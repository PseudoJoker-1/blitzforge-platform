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

if not exist "build\v3_operation_permissions" (
    mkdir "build\v3_operation_permissions"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX /c ^
    /Iinclude ^
    src\v3\wotb_mod_v3_runtime.cpp ^
    /Fo"build\v3_operation_permissions\runtime.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX /c ^
    /Iinclude ^
    src\v3\client_services.cpp ^
    /Fo"build\v3_operation_permissions\client_services.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX /c ^
    /Iinclude ^
    src\v3\runtime_services.cpp ^
    /Fo"build\v3_operation_permissions\runtime_services.obj"
if not "%errorlevel%"=="0" goto :failed

lib /nologo ^
    /OUT:"build\v3_operation_permissions\runtime_support.lib" ^
    "build\wotb_mod_runtime.lib" ^
    /REMOVE:"build\v3\client_services.obj" ^
    /REMOVE:"build\v3\wotb_mod_v3_runtime.obj" ^
    /REMOVE:"build\v3\runtime_services.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_operation_permissions_tests.cpp ^
    "build\v3_operation_permissions\runtime.obj" ^
    "build\v3_operation_permissions\client_services.obj" ^
    "build\v3_operation_permissions\runtime_services.obj" ^
    "build\v3_operation_permissions\runtime_support.lib" ^
    /Fo"build\v3_operation_permissions\test.obj" ^
    /link /OUT:"build\v3_operation_permissions\test.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_operation_permissions\test.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 operation permission tests passed ===
popd
exit /b 0

:failed
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%
