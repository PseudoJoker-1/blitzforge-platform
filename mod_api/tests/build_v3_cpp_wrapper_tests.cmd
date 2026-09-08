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

if not exist "build\v3_cpp_wrapper_focus" (
    mkdir "build\v3_cpp_wrapper_focus"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_cpp_wrapper_tests.cpp ^
    src\v3\wotb_mod_v3_runtime.cpp ^
    /Fo"build\v3_cpp_wrapper_focus\\" ^
    /link /OUT:"build\v3_cpp_wrapper_tests.exe" ^
    /IMPLIB:"build\v3_cpp_wrapper_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_cpp_wrapper_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 C++ wrapper focused tests passed ===
popd
exit /b 0

:failed
echo V3 C++ wrapper focused tests failed.
popd
exit /b 1

