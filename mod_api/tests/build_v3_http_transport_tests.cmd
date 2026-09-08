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

if not exist "build\v3_http_transport" mkdir "build\v3_http_transport"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /DWOTBMOD_V3_HTTP_STANDALONE_TEST ^
    /Iinclude ^
    tests\v3_http_transport_tests.cpp ^
    src\v3\wotb_mod_v3_runtime.cpp ^
    src\v3\runtime_services.cpp ^
    src\v3\ges_services.cpp ^
    src\v3\ges_schemas.cpp ^
    /Fo"build\v3_http_transport\\" ^
    /link /OUT:"build\v3_http_transport_tests.exe" ^
    /IMPLIB:"build\v3_http_transport_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_http_transport_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 HTTP transport tests passed ===
popd
exit /b 0

:failed
echo V3 HTTP transport tests failed.
popd
exit /b 1
