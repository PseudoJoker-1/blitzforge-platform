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

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude ^
    examples\native_validation_mod\native_validation_mod.cpp ^
    user32.lib ^
    /Fo"build\native_validation_mod.obj" ^
    /link /OUT:"build\native_validation_mod.dll" ^
    /IMPLIB:"build\native_validation_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\native_validation_host_tests.cpp ^
    /Fo"build\native_validation_host_tests.obj" ^
    /link /OUT:"build\native_validation_host_tests.exe" ^
    /IMPLIB:"build\native_validation_host_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\native_validation_host_tests.exe" "build\native_validation_mod.dll"
if not "%errorlevel%"=="0" goto :failed

echo Native validation host contract passed.
popd
exit /b 0

:failed
echo Native validation host contract failed.
popd
exit /b 1
