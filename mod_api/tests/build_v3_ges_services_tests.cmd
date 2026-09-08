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
if not exist "build\v3_ges_tests\mods" mkdir "build\v3_ges_tests\mods"
if not exist "build\v3_ges_tests\cache" mkdir "build\v3_ges_tests\cache"
if not exist "build\v3_ges_tests\config" mkdir "build\v3_ges_tests\config"
cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_ges_services_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_ges_services_tests.obj" ^
    /link /OUT:"build\v3_ges_services_tests.exe"
if not "%errorlevel%"=="0" goto :failed
"build\v3_ges_services_tests.exe"
if not "%errorlevel%"=="0" goto :failed
popd
exit /b 0
:failed
popd
exit /b 1
