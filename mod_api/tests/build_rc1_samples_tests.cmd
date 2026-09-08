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
if not exist "build\rc1_samples" mkdir "build\rc1_samples"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude examples\sample_ui_transaction\sample_ui_transaction.cpp ^
    /Fo"build\rc1_samples\sample_ui_transaction.obj" ^
    /link /OUT:"build\rc1_samples\sample_ui_transaction.dll" ^
    /IMPLIB:"build\rc1_samples\sample_ui_transaction.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude examples\sample_vehicle_cosmetic\sample_vehicle_cosmetic.cpp ^
    /Fo"build\rc1_samples\sample_vehicle_cosmetic.obj" ^
    /link /OUT:"build\rc1_samples\sample_vehicle_cosmetic.dll" ^
    /IMPLIB:"build\rc1_samples\sample_vehicle_cosmetic.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude examples\sample_camera_render\sample_camera_render.cpp ^
    /Fo"build\rc1_samples\sample_camera_render.obj" ^
    /link /OUT:"build\rc1_samples\sample_camera_render.dll" ^
    /IMPLIB:"build\rc1_samples\sample_camera_render.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude tests\rc1_samples_host_tests.cpp ^
    /Fo"build\rc1_samples\rc1_samples_host_tests.obj" ^
    /link /OUT:"build\rc1_samples\rc1_samples_host_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\rc1_samples\rc1_samples_host_tests.exe" ^
    "build\rc1_samples\sample_ui_transaction.dll" ^
    "build\rc1_samples\sample_vehicle_cosmetic.dll" ^
    "build\rc1_samples\sample_camera_render.dll"
if not "%errorlevel%"=="0" goto :failed

echo RC1 sample host tests passed.
popd
exit /b 0

:failed
echo RC1 sample host tests failed.
popd
exit /b 1
