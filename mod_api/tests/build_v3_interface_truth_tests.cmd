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
    /Fo"build\v3_interface_truth_runtime.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c tests\v3_interface_truth_tests.cpp ^
    /Fo"build\v3_interface_truth_tests.obj"
if not "%errorlevel%"=="0" goto :failed

link /nologo ^
    "build\v3_interface_truth_runtime.obj" ^
    "build\v3_interface_truth_tests.obj" ^
    /OUT:"build\v3_interface_truth_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_interface_truth_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 interface truth tests passed ===
popd
exit /b 0

:failed
echo V3 interface truth tests failed.
popd
exit /b 1
