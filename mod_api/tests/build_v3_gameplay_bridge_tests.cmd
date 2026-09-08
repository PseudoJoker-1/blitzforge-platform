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

cl /nologo /O2 /MD /W3 /WX- ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    /c "..\proxy_dll\third_party\minhook\src\buffer.c" ^
    /Fo"build\v3_bridge_minhook_buffer.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /O2 /MD /W3 /WX- ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    /c "..\proxy_dll\third_party\minhook\src\hook.c" ^
    /Fo"build\v3_bridge_minhook_hook.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /O2 /MD /W3 /WX- ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    /c "..\proxy_dll\third_party\minhook\src\trampoline.c" ^
    /Fo"build\v3_bridge_minhook_trampoline.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /O2 /MD /W3 /WX- ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    /c "..\proxy_dll\third_party\minhook\src\hde\hde32.c" ^
    /Fo"build\v3_bridge_minhook_hde32.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /c tests\v3_gameplay_bridge_tests.cpp ^
    /Fo"build\v3_gameplay_bridge_tests.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /c loader\v3_camera_effects.cpp ^
    /Fo"build\v3_camera_effects.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /c loader\v3_hook_observers.cpp ^
    /Fo"build\v3_bridge_hook_observers.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /permissive- /W4 /WX ^
    /Iinclude ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /c loader\v3_native_bindings.cpp ^
    /Fo"build\v3_gameplay_bridge_bindings.obj"
if not "%errorlevel%"=="0" goto :failed

link /nologo ^
    "build\v3_gameplay_bridge_tests.obj" ^
    "build\v3_camera_effects.obj" ^
    "build\v3_bridge_hook_observers.obj" ^
    "build\v3_gameplay_bridge_bindings.obj" ^
    "build\v3_bridge_minhook_buffer.obj" ^
    "build\v3_bridge_minhook_hook.obj" ^
    "build\v3_bridge_minhook_trampoline.obj" ^
    "build\v3_bridge_minhook_hde32.obj" ^
    bcrypt.lib d3d11.lib d3dcompiler.lib dxgi.lib user32.lib ^
    /OUT:"build\v3_gameplay_bridge_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\v3_gameplay_bridge_tests.exe"
if not "%errorlevel%"=="0" goto :failed

echo === V3 safe gameplay bridge tests passed ===
popd
exit /b 0

:failed
echo V3 safe gameplay bridge tests failed.
popd
exit /b 1
