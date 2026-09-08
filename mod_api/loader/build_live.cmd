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
if not exist "build\live_env" mkdir "build\live_env"
if not exist "build\live_env\mods" mkdir "build\live_env\mods"
if not exist "build\live_env\mods\wotbmod.native_validation\bin\windows-x86" mkdir "build\live_env\mods\wotbmod.native_validation\bin\windows-x86"
if not exist "build\live_env\mods\data\wotbmod.native_validation" mkdir "build\live_env\mods\data\wotbmod.native_validation"
if not exist "build\live_env\mods\data\native_live_test_mod\fixtures" mkdir "build\live_env\mods\data\native_live_test_mod\fixtures"
if not exist "build\live_env\mods\data\vehicle_skin_test_mod\skin\low" mkdir "build\live_env\mods\data\vehicle_skin_test_mod\skin\low"
if not exist "build\live_env\mods\data\vehicle_skin_test_mod\skin\high" mkdir "build\live_env\mods\data\vehicle_skin_test_mod\skin\high"
if not exist "build\wotb_mod_runtime.lib" (
    echo Missing build\wotb_mod_runtime.lib. Run build.cmd first.
    goto :failed
)

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\native_live_test_mod\native_live_test_mod.cpp ^
    /Fo"build\native_live_test_mod.obj" ^
    /link /OUT:"build\native_live_test_mod.dll" /IMPLIB:"build\native_live_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\api_selftest_mod\api_selftest_mod.cpp ^
    /Fo"build\api_selftest_mod.obj" ^
    /link /OUT:"build\api_selftest_mod.dll" /IMPLIB:"build\api_selftest_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\custom_audio_test_mod\custom_audio_test_mod.cpp ^
    /Fo"build\custom_audio_test_mod.obj" ^
    /link /OUT:"build\custom_audio_test_mod.dll" /IMPLIB:"build\custom_audio_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\new_gameplay_events_test_mod\new_gameplay_events_test_mod.cpp ^
    /Fo"build\new_gameplay_events_test_mod.obj" ^
    /link /OUT:"build\new_gameplay_events_test_mod.dll" /IMPLIB:"build\new_gameplay_events_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\vehicle_skin_test_mod\vehicle_skin_test_mod.cpp ^
    /Fo"build\vehicle_skin_test_mod.obj" ^
    /link /OUT:"build\vehicle_skin_test_mod.dll" /IMPLIB:"build\vehicle_skin_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\object260_t3485_model_mod\object260_t3485_model_mod.cpp ^
    /Fo"build\object260_t3485_model_mod.obj" ^
    /link /OUT:"build\object260_t3485_model_mod.dll" /IMPLIB:"build\object260_t3485_model_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /utf-8 /LD ^
    /Iinclude ^
    examples\native_validation_mod\native_validation_mod.cpp ^
    user32.lib ^
    /Fo"build\native_validation_mod.obj" ^
    /link /OUT:"build\native_validation_mod.dll" ^
    /IMPLIB:"build\native_validation_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /DWOTBMOD_DEVELOPER_UNSAFE_LEGACY_API=0 ^
    /Iinclude ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    loader\wotb_mod_loader.cpp ^
    loader\v3_camera_effects.cpp ^
    loader\v3_native_bindings.cpp ^
    loader\v3_native_client_services.cpp ^
    loader\v3_native_ges.cpp ^
    loader\v3_native_session_cluster.cpp ^
    loader\v3_hook_observers.cpp ^
    loader\v3_managed_renderer.cpp ^
    "..\proxy_dll\third_party\minhook\src\buffer.c" ^
    "..\proxy_dll\third_party\minhook\src\hook.c" ^
    "..\proxy_dll\third_party\minhook\src\trampoline.c" ^
    "..\proxy_dll\third_party\minhook\src\hde\hde32.c" ^
    "build\wotb_mod_runtime.lib" ^
    xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib ^
    d3d11.lib d3dcompiler.lib dxgi.lib bcrypt.lib user32.lib ^
    /Fo"build\\" ^
    /link /OUT:"build\wotb_mod_loader.dll" /IMPLIB:"build\wotb_mod_loader.lib" /MAP:"build\wotb_mod_loader.map"
if not "%errorlevel%"=="0" goto :failed

python -B tests\test_live_loader_freshness.py
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    loader\live_test_launcher.cpp ^
    /Fo"build\live_test_launcher.obj" ^
    /link /OUT:"build\live_test_launcher.exe"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /DWOTBMOD_GAME_MODS_LAUNCHER ^
    loader\live_test_launcher.cpp ^
    /Fo"build\wotb_mod_launcher.obj" ^
    /link /OUT:"build\wotb_mod_launcher.exe"
if not "%errorlevel%"=="0" goto :failed

copy /y "build\native_live_test_mod.dll" "build\live_env\mods\native_live_test_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\api_selftest_mod.dll" "build\live_env\mods\api_selftest_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\custom_audio_test_mod.dll" "build\live_env\mods\custom_audio_test_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\new_gameplay_events_test_mod.dll" "build\live_env\mods\new_gameplay_events_test_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\vehicle_skin_test_mod.dll" "build\live_env\mods\vehicle_skin_test_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\object260_t3485_model_mod.dll" "build\live_env\mods\object260_t3485_model_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "build\native_validation_mod.dll" "build\live_env\mods\wotbmod.native_validation\bin\windows-x86\native_validation_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "examples\native_validation_mod\manifest.json" "build\live_env\mods\wotbmod.native_validation\manifest.json" >nul
if not "%errorlevel%"=="0" goto :failed
if not exist "..\mods\native-validation\files\mods\wotbmod.native_validation\bin\windows-x86" mkdir "..\mods\native-validation\files\mods\wotbmod.native_validation\bin\windows-x86"
copy /y "build\native_validation_mod.dll" "..\mods\native-validation\files\mods\wotbmod.native_validation\bin\windows-x86\native_validation_mod.dll" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "examples\native_validation_mod\manifest.json" "..\mods\native-validation\files\mods\wotbmod.native_validation\manifest.json" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "loader\fixtures\probe.yaml" "build\live_env\mods\data\native_live_test_mod\fixtures\probe.yaml" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "loader\fixtures\probe.bin" "build\live_env\mods\data\native_live_test_mod\fixtures\probe.bin" >nul
if not "%errorlevel%"=="0" goto :failed
if exist "build\live_env\mods\data\native_live_test_mod\fixtures\probe.sc2" del /q "build\live_env\mods\data\native_live_test_mod\fixtures\probe.sc2"
if exist "build\live_env\mods\data\native_live_test_mod\fixtures\probe.sc2.dvpl" del /q "build\live_env\mods\data\native_live_test_mod\fixtures\probe.sc2.dvpl"
set "PYTHON_EXE=..\..\.venv\Scripts\python.exe"
if not exist "%PYTHON_EXE%" set "PYTHON_EXE=python"
"%PYTHON_EXE%" "loader\make_probe_archive.py" "build\live_env\mods\data\native_live_test_mod\fixtures\probe.zip"
if not "%errorlevel%"=="0" goto :failed
"%PYTHON_EXE%" "loader\unpack_dvpl.py" "..\..\Data\3d\Tanks\USSR\T-34-85.sc2.dvpl" "build\live_env\mods\data\native_live_test_mod\fixtures\probe.sc2"
if not "%errorlevel%"=="0" goto :failed
copy /y "loader\fixtures\probe.tex" "build\live_env\mods\data\native_live_test_mod\fixtures\probe.tex" >nul
if not "%errorlevel%"=="0" goto :failed
if exist "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.sc2" del /q "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.sc2"
if exist "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.dx11.dds" del /q "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.dx11.dds"
copy /y "..\..\Data\3d\Tanks\USSR\T-34-85.sc2.dvpl" "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.sc2.dvpl" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "examples\vehicle_skin_test_mod\fixtures\low\T-34-85.material.yaml" "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.material.yaml" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "..\..\Data\3d\Tanks\USSR\images\T-34-85.dx11.dds.dvpl" "build\live_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.dx11.dds.dvpl" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "..\..\Data\3d\Tanks\USSR\T-34-85.sc2.dvpl" "build\live_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.sc2.dvpl" >nul
if not "%errorlevel%"=="0" goto :failed
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.material.yaml" "build\live_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.material.yaml" >nul
if not "%errorlevel%"=="0" goto :failed
"%PYTHON_EXE%" "examples\vehicle_skin_test_mod\generate_checker_dds.py" "build\live_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.dx11.dds"
if not "%errorlevel%"=="0" goto :failed

(
    echo [mods]
    echo test.native-live=1
    echo test.api-selftest=1
    echo test.custom-audio=1
    echo new_gameplay_events_test_mod=1
    echo vehicle_skin_test_mod=1
    echo object260_t3485_model_mod=1
    echo wotbmod.native_validation=1
    echo.
    echo [permissions]
    echo wotbmod.native_validation=3
) > "build\live_env\mods\mods.ini"

echo === Live loader and native client test built ===
echo Loader: build\wotb_mod_loader.dll
echo Live test: build\live_test_launcher.exe
echo Game mods: build\wotb_mod_launcher.exe
echo Validation package: build\live_env\mods\wotbmod.native_validation
python ..\modpack.py build ..\mods\native-validation ..\dist\native-validation-1.1.0.zip
if not "%errorlevel%"=="0" goto :failed
echo Catalog artifact: ..\dist\native-validation-1.1.0.zip
popd
exit /b 0

:failed
echo Live loader build failed.
popd
exit /b 1
