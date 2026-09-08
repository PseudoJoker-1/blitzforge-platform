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
if not exist "build\lua" mkdir "build\lua"

rem /D_CRT_SECURE_NO_WARNINGS: Lua uses fopen/strcpy throughout and is not ours
rem to change. /wd4334 and /wd4310 silence two shifts and a cast that are
rem deliberate in Lua's own code.
cl /nologo /c /O2 /MD /D_CRT_SECURE_NO_WARNINGS /wd4334 /wd4310 ^
    third_party\lua\*.c ^
    /Fo"build\lua\\"
if not "%errorlevel%"=="0" goto :failed

lib /nologo /OUT:"build\lua54.lib" "build\lua\*.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    tests\lua_vendor_tests.cpp ^
    "build\lua54.lib" ^
    /Fo"build\lua_vendor_tests.obj" ^
    /link /OUT:"build\lua_vendor_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\lua_vendor_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo Lua vendor check passed.
popd
exit /b 0

:failed
echo Lua vendor check failed.
popd
exit /b 1
