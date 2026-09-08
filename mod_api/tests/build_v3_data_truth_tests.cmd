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

if not exist "build\v3_data_truth" mkdir "build\v3_data_truth"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_data_truth_tests.cpp ^
    src\v3\wotb_mod_v3_runtime.cpp ^
    src\v3\runtime_services.cpp ^
    src\v3\ges_services.cpp ^
    src\v3\ges_schemas.cpp ^
    src\v3\data_services.cpp ^
    src\v3\dava_native_registry.cpp ^
    src\v3\dvpl_decoder.cpp ^
    /Fo"build\v3_data_truth\\" ^
    /link /OUT:"build\v3_data_truth_tests.exe" ^
    /IMPLIB:"build\v3_data_truth_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_data_truth_tests.exe"
if not "%errorlevel%"=="0" goto :failed

popd
exit /b 0

:failed
echo V3 data truth tests failed.
popd
exit /b 1
