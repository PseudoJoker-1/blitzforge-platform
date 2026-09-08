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
if not exist "build\v3_session_cluster_tests\mods" mkdir "build\v3_session_cluster_tests\mods"
if not exist "build\v3_session_cluster_tests\cache" mkdir "build\v3_session_cluster_tests\cache"
if not exist "build\v3_session_cluster_tests\config" mkdir "build\v3_session_cluster_tests\config"
if not exist "build\v3_session_cluster_native" mkdir "build\v3_session_cluster_native"
cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_session_cluster_services_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_session_cluster_services_tests.obj" ^
    /link /OUT:"build\v3_session_cluster_services_tests.exe"
if not "%errorlevel%"=="0" goto :failed
"build\v3_session_cluster_services_tests.exe"
if not "%errorlevel%"=="0" goto :failed
if not exist "tests\v3_session_cluster_native_tests.cpp" goto :done
cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    tests\v3_session_cluster_native_tests.cpp ^
    loader\v3_native_session_cluster.cpp ^
    /Fo"build\v3_session_cluster_native\\" ^
    /link /OUT:"build\v3_session_cluster_native_tests.exe"
if not "%errorlevel%"=="0" goto :failed
"build\v3_session_cluster_native_tests.exe"
if not "%errorlevel%"=="0" goto :failed
:done
popd
exit /b 0
:failed
popd
exit /b 1
