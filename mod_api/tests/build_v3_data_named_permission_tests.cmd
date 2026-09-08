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

if not exist "build\v3_data_named_permissions" (
    mkdir "build\v3_data_named_permissions"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    /c src\v3\wotb_mod_v3_runtime.cpp ^
    /Fo"build\v3_data_named_permissions\runtime.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    /c src\v3\data_services.cpp ^
    /Fo"build\v3_data_named_permissions\data_services.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    /c src\v3\dava_native_registry.cpp ^
    /Fo"build\v3_data_named_permissions\dava_native_registry.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_data_named_permission_tests.cpp ^
    "build\v3_data_named_permissions\runtime.obj" ^
    "build\v3_data_named_permissions\data_services.obj" ^
    "build\v3_data_named_permissions\dava_native_registry.obj" ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_data_named_permissions\test.obj" ^
    /link /OUT:"build\v3_data_named_permissions\test.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_data_named_permissions\test.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 data named permission tests passed ===
popd
exit /b 0

:failed
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%
