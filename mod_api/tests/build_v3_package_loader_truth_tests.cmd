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

if not exist "build\v3_package_loader_truth" (
    mkdir "build\v3_package_loader_truth"
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    /c src\v3\package_loader.cpp ^
    /Fo"build\v3_package_loader_truth\package_loader.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_package_loader_tests.cpp ^
    "build\v3_package_loader_truth\package_loader.obj" ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_package_loader_truth\test.obj" ^
    /link /OUT:"build\v3_package_loader_truth\test.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_package_loader_truth\test.exe" ^
    "build\v3_package_loader_truth\env"
if not "%errorlevel%"=="0" goto :failed

echo === V3 package loader truth tests passed ===
popd
exit /b 0

:failed
set "STATUS=%ERRORLEVEL%"
popd
exit /b %STATUS%
