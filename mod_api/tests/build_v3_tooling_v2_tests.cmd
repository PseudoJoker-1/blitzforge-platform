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

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c src\v3\wotb_mod_v3_runtime.cpp ^
    /Fo"build\v3_tooling_v2_runtime.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c src\v3\tooling_services.cpp ^
    /Fo"build\v3_tooling_v2_services.obj"
if not "%errorlevel%"=="0" goto :failed

rem The scene and material inspectors call the installed DAVA native registry.
rem Nothing installs a backend in this binary - that is deliberate, it is how
rem the "no native enumeration backend" refusal gets exercised - but the symbols
rem still have to resolve, so the registry itself is linked in.
cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c src\v3\dava_native_registry.cpp ^
    /Fo"build\v3_tooling_v2_dava_registry.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c tests\v3_tooling_v2_tests.cpp ^
    /Fo"build\v3_tooling_v2_tests.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /TC /std:c11 /W4 /WX ^
    /Iinclude ^
    /c tests\v3_tooling_v2_abi_c.c ^
    /Fo"build\v3_tooling_v2_abi_c.obj"
if not "%errorlevel%"=="0" goto :failed

link /nologo ^
    "build\v3_tooling_v2_runtime.obj" ^
    "build\v3_tooling_v2_services.obj" ^
    "build\v3_tooling_v2_dava_registry.obj" ^
    "build\v3_tooling_v2_tests.obj" ^
    /OUT:"build\v3_tooling_v2_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_tooling_v2_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 tooling V2 tests passed ===
popd
exit /b 0

:failed
echo V3 tooling V2 tests failed.
popd
exit /b 1
