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
if not exist "build\v3_ges_native" mkdir "build\v3_ges_native"
cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_ges_native_tests.cpp ^
    loader\v3_native_ges.cpp ^
    src\v3\ges_schemas.cpp ^
    /Fo"build\v3_ges_native\\" ^
    /link /OUT:"build\v3_ges_native_tests.exe"
if not "%errorlevel%"=="0" goto :failed
"build\v3_ges_native_tests.exe"
if not "%errorlevel%"=="0" goto :failed
popd
exit /b 0
:failed
popd
exit /b 1
