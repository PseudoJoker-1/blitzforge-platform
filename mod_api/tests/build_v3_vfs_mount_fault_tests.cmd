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
if not exist "build\v3_vfs_mount_fault" mkdir "build\v3_vfs_mount_fault"

rem src\v3\data_services.cpp is deliberately absent from this compile line.
rem The test #includes it so that it can reach g_vfs_mutex and g_mounts,
rem which have internal linkage; compiling it again here would duplicate
rem every symbol it defines.
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_vfs_mount_fault_tests.cpp ^
    src\v3\wotb_mod_v3_runtime.cpp ^
    src\v3\runtime_services.cpp ^
    src\v3\ges_services.cpp ^
    src\v3\ges_schemas.cpp ^
    src\v3\dava_native_registry.cpp ^
    src\v3\dvpl_decoder.cpp ^
    /Fo"build\v3_vfs_mount_fault\\" ^
    /link /OUT:"build\v3_vfs_mount_fault_tests.exe" ^
    /IMPLIB:"build\v3_vfs_mount_fault_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_vfs_mount_fault_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo V3 VFS mount fault regression passed.
popd
exit /b 0

:failed
echo V3 VFS mount fault regression failed.
popd
exit /b 1
