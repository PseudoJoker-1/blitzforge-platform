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
if not exist "build\rc1" mkdir "build\rc1"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_rc1_contract_probe.cpp ^
    /Fo"build\rc1\v3_rc1_contract_probe.obj" ^
    /link /OUT:"build\rc1\v3_rc1_contract_probe.exe"
if not "%errorlevel%"=="0" goto :failed

"build\rc1\v3_rc1_contract_probe.exe"
if not "%errorlevel%"=="0" goto :failed

python tools\verify_rc1_contract.py
if not "%errorlevel%"=="0" goto :failed

echo V3 RC1 frozen-contract tests passed.
popd
exit /b 0

:failed
echo V3 RC1 frozen-contract tests failed.
popd
exit /b 1
