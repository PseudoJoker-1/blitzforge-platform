@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"
if not exist "%VCVARS%" (
    echo Visual Studio 2022 C++ tools were not found.
    popd
    exit /b 1
)

call "%VCVARS%" >nul
if not "%errorlevel%"=="0" (
    echo vcvars32 failed.
    popd
    exit /b 1
)

if not exist "build" mkdir "build"
if not exist "build\v3" mkdir "build\v3"
if exist "build\v3\*.obj" del /q "build\v3\*.obj"
if not exist "build\smoke_env" mkdir "build\smoke_env"
if not exist "build\smoke_env\mods" mkdir "build\smoke_env\mods"
if not exist "build\smoke_env\mods\data\hello_mod\resources" mkdir "build\smoke_env\mods\data\hello_mod\resources"
if not exist "build\smoke_env\mods\data\resource_mod\low" mkdir "build\smoke_env\mods\data\resource_mod\low"
if not exist "build\smoke_env\mods\data\resource_mod\high" mkdir "build\smoke_env\mods\data\resource_mod\high"
copy /y "tests\fixtures\example.yaml.dvpl" "build\smoke_env\mods\data\hello_mod\resources\example.yaml.dvpl" >nul
if errorlevel 1 goto :failed
copy /y "tests\fixtures\low\value.txt" "build\smoke_env\mods\data\resource_mod\low\value.txt" >nul
copy /y "tests\fixtures\high\value.txt" "build\smoke_env\mods\data\resource_mod\high\value.txt" >nul
copy /y "tests\fixtures\all_enabled.ini" "build\smoke_env\mods\mods.ini" >nul

if not exist "build\api_env\mods\data\hello_mod\resources" mkdir "build\api_env\mods\data\hello_mod\resources"
if not exist "build\api_env\mods\data\resource_mod\low" mkdir "build\api_env\mods\data\resource_mod\low"
if not exist "build\api_env\mods\data\resource_mod\high" mkdir "build\api_env\mods\data\resource_mod\high"
if not exist "build\api_env\mods\data\api_contract_mod\low" mkdir "build\api_env\mods\data\api_contract_mod\low"
if not exist "build\api_env\mods\data\api_contract_mod\high" mkdir "build\api_env\mods\data\api_contract_mod\high"
if not exist "build\api_env\mods\data\api_contract_mod\dvpl" mkdir "build\api_env\mods\data\api_contract_mod\dvpl"
copy /y "tests\fixtures\example.yaml.dvpl" "build\api_env\mods\data\hello_mod\resources\example.yaml.dvpl" >nul
if errorlevel 1 goto :failed
copy /y "tests\fixtures\low\value.txt" "build\api_env\mods\data\resource_mod\low\value.txt" >nul
copy /y "tests\fixtures\high\value.txt" "build\api_env\mods\data\resource_mod\high\value.txt" >nul
copy /y "tests\fixtures\contract\low\value.txt" "build\api_env\mods\data\api_contract_mod\low\value.txt" >nul
copy /y "tests\fixtures\contract\high\value.txt" "build\api_env\mods\data\api_contract_mod\high\value.txt" >nul
copy /y "tests\fixtures\contract\dvpl\packed.yaml.dvpl" "build\api_env\mods\data\api_contract_mod\dvpl\packed.yaml.dvpl" >nul
if errorlevel 1 goto :failed
copy /y "tests\fixtures\all_enabled.ini" "build\api_env\mods\mods.ini" >nul

if not exist "build\empty_env\mods" mkdir "build\empty_env\mods"
if exist "build\empty_env\mods\*.dll" del /q "build\empty_env\mods\*.dll"
if not exist "build\malformed_env\mods" mkdir "build\malformed_env\mods"
if not exist "build\published_selftest_env\mods" mkdir "build\published_selftest_env\mods"
if not exist "build\new_api_env\mods" mkdir "build\new_api_env\mods"
if not exist "build\safe_mode_env\mods" mkdir "build\safe_mode_env\mods"
for %%D in (smoke_env api_env empty_env malformed_env published_selftest_env new_api_env safe_mode_env) do (
    if exist "build\%%D\mods\cache\runtime_session.marker" del /q "build\%%D\mods\cache\runtime_session.marker"
    if exist "build\%%D\mods\cache\runtime_session.marker.tmp" del /q "build\%%D\mods\cache\runtime_session.marker.tmp"
    if exist "build\%%D\mods\cache\crash_history.ini" del /q "build\%%D\mods\cache\crash_history.ini"
    if exist "build\%%D\mods\cache\auto_disabled_mod.ini" del /q "build\%%D\mods\cache\auto_disabled_mod.ini"
)
if exist "build\safe_mode_env\mods\*.dll" del /q "build\safe_mode_env\mods\*.dll"
if exist "build\safe_mode_env\mods\portable_safe_mode" rmdir /s /q "build\safe_mode_env\mods\portable_safe_mode"
if not exist "build\new_api_env\Data\3d\Tanks\USSR" mkdir "build\new_api_env\Data\3d\Tanks\USSR"
if not exist "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\low" mkdir "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\low"
if not exist "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\high" mkdir "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\high"
copy /y "examples\vehicle_skin_test_mod\fixtures\low\T-34-85.sc2" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.sc2" >nul
if errorlevel 1 goto :failed
copy /y "examples\vehicle_skin_test_mod\fixtures\low\T-34-85.material.yaml" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.material.yaml" >nul
copy /y "examples\vehicle_skin_test_mod\fixtures\low\T-34-85.dx11.dds" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\low\T-34-85.dx11.dds" >nul
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.sc2" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.sc2" >nul
if errorlevel 1 goto :failed
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.material.yaml" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.material.yaml" >nul
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.dx11.dds" "build\new_api_env\mods\data\vehicle_skin_test_mod\skin\high\T-34-85.dx11.dds" >nul
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.sc2" "build\new_api_env\Data\3d\Tanks\USSR\T-34-85.sc2.dvpl" >nul
if errorlevel 1 goto :failed
copy /y "examples\vehicle_skin_test_mod\fixtures\high\T-34-85.sc2" "build\new_api_env\Data\3d\Tanks\USSR\T-34-85.scg.dvpl" >nul
if errorlevel 1 goto :failed
(
    echo [mods]
    echo new_gameplay_events_test_mod=1
    echo vehicle_skin_test_mod=1
    echo object260_t3485_model_mod=1
) > "build\new_api_env\mods\mods.ini"

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /Iinclude ^
    src\wotb_mod_runtime.cpp ^
    /Fo"build\wotb_mod_runtime.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /Iinclude ^
    src\wotb_mod_windows_audio.cpp ^
    /Fo"build\wotb_mod_windows_audio.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /Iinclude ^
    src\wotb_mod_dava_sound.cpp ^
    /Fo"build\wotb_mod_dava_sound.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /Iinclude ^
    src\wotb_mod_dava_resources.cpp ^
    /Fo"build\wotb_mod_dava_resources.obj"
if not "%errorlevel%"=="0" goto :failed

rem Private DAVA entrypoints compile even when their __thiscall argument shape
rem is wrong. Check the exact client machine code as well as the wrapper source
rem so a missing stack argument or FastName ownership regression fails the
rem build before it reaches the live game.
python -B tests\test_dava_native_exact_abi.py
if not "%errorlevel%"=="0" goto :failed

for %%F in (src\v3\*.cpp) do (
    cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /c ^
        /Iinclude ^
        "%%F" ^
        /Fo"build\v3\%%~nF.obj"
    if not "!errorlevel!"=="0" goto :failed
)

rem The loader is the DLL that gets injected into the player's game, and
rem until this gate existed nothing here compiled it: only loader\build_live.cmd
rem did, and that is never called from this script. A constant dropped or
rem misspelled in wotb_mod_loader.cpp therefore left the whole suite green and
rem surfaced for the first time in someone's game.
rem
rem Compile-only on purpose. build_live.cmd links it, but it also copies out of
rem the game's Data\ tree, writes into ..\mods\native-validation\files\ and runs
rem modpack.py -- side effects a test run must not have. Same flags build_live
rem uses, minus /LD, plus /c.
if not exist "build\loader" mkdir "build\loader"
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /c ^
    /DWOTBMOD_DEVELOPER_UNSAFE_LEGACY_API=0 ^
    /Iinclude ^
    /I"..\proxy_dll\third_party\minhook\include" ^
    /I"..\proxy_dll\third_party\minhook\src\hde" ^
    loader\wotb_mod_loader.cpp ^
    loader\v3_camera_effects.cpp ^
    loader\v3_native_bindings.cpp ^
    loader\v3_native_client_services.cpp ^
    loader\v3_native_ges.cpp ^
    loader\v3_hook_observers.cpp ^
    loader\v3_managed_renderer.cpp ^
    /Fo"build\loader\\"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /TC /W4 /c ^
    /Iinclude ^
    tests\abi_c_compile.c ^
    /Fo"build\abi_c_compile.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /TC /std:c11 /W4 /WX /c ^
    /Iinclude ^
    tests\v3_core_abi_c.c ^
    /Fo"build\v3_core_abi_c.obj"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /c ^
    /Iinclude ^
    tests\v3_client_services_compile.cpp ^
    /Fo"build\v3_client_services_compile.obj"
if not "%errorlevel%"=="0" goto :failed

> "build\v3_objects.rsp" (
    for %%F in (build\v3\*.obj) do echo "%%F"
)

lib /nologo /OUT:"build\wotb_mod_runtime.lib" ^
    "build\wotb_mod_runtime.obj" ^
    "build\wotb_mod_windows_audio.obj" ^
    "build\wotb_mod_dava_sound.obj" ^
    "build\wotb_mod_dava_resources.obj" ^
    @"build\v3_objects.rsp"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX ^
    /Iinclude ^
    tests\v3_core_runtime_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_core_runtime_tests.obj" ^
    /link /OUT:"build\v3_core_runtime_tests.exe" ^
    /IMPLIB:"build\v3_core_runtime_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_core_runtime_tests.exe" "build\v3_core_env"
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_cpp_wrapper_tests.cmd
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /DWOTBMOD_V3_DATA_SERVICES_STANDALONE_TEST ^
    /Iinclude ^
    tests\v3_data_services_compile.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_data_services_preflight_tests.obj" ^
    /link /OUT:"build\v3_data_services_preflight_tests.exe" ^
    /IMPLIB:"build\v3_data_services_preflight_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_data_services_preflight_tests.exe"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_package_loader_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_package_loader_tests.obj" ^
    /link /OUT:"build\v3_package_loader_tests.exe" ^
    /IMPLIB:"build\v3_package_loader_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_package_loader_tests.exe" ^
    "build\v3_package_loader_env"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX ^
    /Iinclude ^
    tests\v3_runtime_services_compile.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_runtime_services_tests.obj" ^
    /link /OUT:"build\v3_runtime_services_tests.exe" ^
    /IMPLIB:"build\v3_runtime_services_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_runtime_services_tests.exe"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /utf-8 /LD ^
    /Iinclude ^
    tests\v3_package_runtime_mod.cpp ^
    /Fo"build\v3_package_runtime_mod.obj" ^
    /link /OUT:"build\v3_package_runtime_mod.dll" ^
    /IMPLIB:"build\v3_package_runtime_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude ^
    tests\v3_package_runtime_loose_mod.cpp ^
    /Fo"build\v3_package_runtime_loose_mod.obj" ^
    /link /OUT:"build\v3_package_runtime_loose_mod.dll" ^
    /IMPLIB:"build\v3_package_runtime_loose_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\v3_package_runtime_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\v3_package_runtime_tests.obj" ^
    /link /OUT:"build\v3_package_runtime_tests.exe" ^
    /IMPLIB:"build\v3_package_runtime_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\v3_package_runtime_tests.exe" ^
    "build\v3_package_runtime_env" ^
    "build\v3_package_runtime_mod.dll" ^
    "build\v3_package_runtime_loose_mod.dll"
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_gameplay_bridge_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_dvpl_decoder_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_data_truth_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_package_loader_truth_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_interface_truth_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_http_transport_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_tooling_v2_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_named_permissions_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_operation_permissions_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_data_named_permission_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_vfs_mount_fault_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_client_truthful_slices_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_ges_services_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_ges_native_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_session_cluster_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_hook_observers_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_post_rc1_services_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_dava_native_registry_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_native_camera_layout_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_native_ui_bridge_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_ui_public_bridge_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_managed_renderer_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_audio_extensions_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_wotbmod_cli_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_native_validation_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_lua_vendor_tests.cmd
if not "%errorlevel%"=="0" goto :failed
call tests\build_lua_host_tests.cmd
if not "%errorlevel%"=="0" goto :failed

powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_lua_host_package.ps1 -SkipBuild
if not "%errorlevel%"=="0" goto :failed

powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_public_preview_bundle.ps1
if not "%errorlevel%"=="0" goto :failed

call tests\build_v3_rc1_contract_tests.cmd
if not "%errorlevel%"=="0" goto :failed

call tests\build_rc1_samples_tests.cmd
if not "%errorlevel%"=="0" goto :failed

powershell -NoProfile -ExecutionPolicy Bypass -File tests\validation_bundle_export_tests.ps1
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude ^
    examples\v3_core_runtime_test_mod\v3_core_runtime_test_mod.cpp ^
    /Fo"build\v3_core_runtime_test_mod.obj" ^
    /link /OUT:"build\v3_core_runtime_test_mod.dll" ^
    /IMPLIB:"build\v3_core_runtime_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude ^
    examples\v3_client_resources_test_mod\v3_client_resources_test_mod.cpp ^
    /Fo"build\v3_client_resources_test_mod.obj" ^
    /link /OUT:"build\v3_client_resources_test_mod.dll" ^
    /IMPLIB:"build\v3_client_resources_test_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    examples\hello_mod\hello_mod.cpp ^
    /Fo"build\hello_mod.obj" ^
    /link /OUT:"build\hello_mod.dll" /IMPLIB:"build\hello_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude ^
    tests\safe_mode_crash_mod.cpp ^
    /Fo"build\safe_mode_crash_mod.obj" ^
    /link /OUT:"build\safe_mode_crash_mod.dll" ^
    /IMPLIB:"build\safe_mode_crash_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\fault_mod.cpp ^
    /Fo"build\fault_mod.obj" ^
    /link /OUT:"build\fault_mod.dll" /IMPLIB:"build\fault_mod.lib"
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

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\resource_mod.cpp ^
    /Fo"build\resource_mod.obj" ^
    /link /OUT:"build\resource_mod.dll" /IMPLIB:"build\resource_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\api_contract_mod.cpp ^
    /Fo"build\api_contract_mod.obj" ^
    /link /OUT:"build\api_contract_mod.dll" /IMPLIB:"build\api_contract_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\api_peer_mod.cpp ^
    /Fo"build\api_peer_mod.obj" ^
    /link /OUT:"build\api_peer_mod.dll" /IMPLIB:"build\api_peer_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\bad_abi_mod.cpp ^
    /Fo"build\bad_abi_mod.obj" ^
    /link /OUT:"build\bad_abi_mod.dll" /IMPLIB:"build\bad_abi_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\duplicate_id_mod.cpp ^
    /Fo"build\duplicate_id_mod.obj" ^
    /link /OUT:"build\duplicate_id_mod.dll" /IMPLIB:"build\duplicate_id_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\invalid_id_mod.cpp ^
    /Fo"build\invalid_id_mod.obj" ^
    /link /OUT:"build\invalid_id_mod.dll" /IMPLIB:"build\invalid_id_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\entry_fault_mod.cpp ^
    /Fo"build\entry_fault_mod.obj" ^
    /link /OUT:"build\entry_fault_mod.dll" /IMPLIB:"build\entry_fault_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\entry_error_mod.cpp ^
    /Fo"build\entry_error_mod.obj" ^
    /link /OUT:"build\entry_error_mod.dll" /IMPLIB:"build\entry_error_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\no_entry_mod.cpp ^
    /Fo"build\no_entry_mod.obj" ^
    /link /OUT:"build\no_entry_mod.dll" /IMPLIB:"build\no_entry_mod.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /LD ^
    /Iinclude ^
    tests\short_info_mod.cpp ^
    /Fo"build\short_info_mod.obj" ^
    /link /OUT:"build\short_info_mod.dll" /IMPLIB:"build\short_info_mod.lib"
if not "%errorlevel%"=="0" goto :failed

copy /y "build\hello_mod.dll" "build\smoke_env\mods\hello_mod.dll" >nul
copy /y "build\fault_mod.dll" "build\smoke_env\mods\fault_mod.dll" >nul
copy /y "build\resource_mod.dll" "build\smoke_env\mods\resource_mod.dll" >nul
copy /y "build\safe_mode_crash_mod.dll" "build\safe_mode_env\mods\safe_mode_crash_mod.dll" >nul

copy /y "build\api_contract_mod.dll" "build\api_env\mods\api_contract_mod.dll" >nul
copy /y "build\api_peer_mod.dll" "build\api_env\mods\api_peer_mod.dll" >nul
copy /y "build\fault_mod.dll" "build\api_env\mods\fault_mod.dll" >nul
copy /y "build\hello_mod.dll" "build\api_env\mods\hello_mod.dll" >nul
copy /y "build\resource_mod.dll" "build\api_env\mods\resource_mod.dll" >nul

copy /y "build\api_contract_mod.dll" "build\malformed_env\mods\api_contract_mod.dll" >nul
copy /y "build\bad_abi_mod.dll" "build\malformed_env\mods\bad_abi_mod.dll" >nul
copy /y "build\duplicate_id_mod.dll" "build\malformed_env\mods\duplicate_id_mod.dll" >nul
copy /y "build\entry_error_mod.dll" "build\malformed_env\mods\entry_error_mod.dll" >nul
copy /y "build\entry_fault_mod.dll" "build\malformed_env\mods\entry_fault_mod.dll" >nul
copy /y "build\invalid_id_mod.dll" "build\malformed_env\mods\invalid_id_mod.dll" >nul
copy /y "build\no_entry_mod.dll" "build\malformed_env\mods\no_entry_mod.dll" >nul
copy /y "build\short_info_mod.dll" "build\malformed_env\mods\short_info_mod.dll" >nul

copy /y "build\api_selftest_mod.dll" "build\published_selftest_env\mods\api_selftest_mod.dll" >nul
copy /y "build\custom_audio_test_mod.dll" "build\published_selftest_env\mods\custom_audio_test_mod.dll" >nul

copy /y "build\new_gameplay_events_test_mod.dll" "build\new_api_env\mods\new_gameplay_events_test_mod.dll" >nul
copy /y "build\vehicle_skin_test_mod.dll" "build\new_api_env\mods\vehicle_skin_test_mod.dll" >nul
copy /y "build\object260_t3485_model_mod.dll" "build\new_api_env\mods\object260_t3485_model_mod.dll" >nul

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\safe_mode_recovery_tests.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\safe_mode_recovery_tests.obj" ^
    /link /OUT:"build\safe_mode_recovery_tests.exe" ^
    /IMPLIB:"build\safe_mode_recovery_tests.lib"
if not "%errorlevel%"=="0" goto :failed

"build\safe_mode_recovery_tests.exe" "build\safe_mode_env"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /Iinclude ^
    tests\smoke_host.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\smoke_host.obj" ^
    /link /OUT:"build\smoke_host.exe" /IMPLIB:"build\smoke_host.lib"
if not "%errorlevel%"=="0" goto :failed

"build\smoke_host.exe" "build\smoke_env"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /Iinclude ^
    tests\api_full_host.cpp ^
    "build\wotb_mod_runtime.lib" ^
    xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib ^
    /Fo"build\api_full_host.obj" ^
    /link /OUT:"build\api_full_host.exe" /IMPLIB:"build\api_full_host.lib"
if not "%errorlevel%"=="0" goto :failed

"build\api_full_host.exe" "build\api_env" "build\empty_env" "build\malformed_env"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /Iinclude ^
    tests\published_selftest_host.cpp ^
    "build\wotb_mod_runtime.lib" ^
    xaudio2.lib mfplat.lib mfreadwrite.lib mfuuid.lib ole32.lib ^
    /Fo"build\published_selftest_host.obj" ^
    /link /OUT:"build\published_selftest_host.exe" /IMPLIB:"build\published_selftest_host.lib"
if not "%errorlevel%"=="0" goto :failed

"build\published_selftest_host.exe" "build\published_selftest_env"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 ^
    /Iinclude ^
    tests\new_api_mods_host.cpp ^
    "build\wotb_mod_runtime.lib" ^
    /Fo"build\new_api_mods_host.obj" ^
    /link /OUT:"build\new_api_mods_host.exe" /IMPLIB:"build\new_api_mods_host.lib"
if not "%errorlevel%"=="0" goto :failed

"build\new_api_mods_host.exe" "build\new_api_env"
if not "%errorlevel%"=="0" goto :failed

echo.
echo === Mod API build, smoke, and full contract tests passed ===
echo Runtime: build\wotb_mod_runtime.lib
echo SDK:     include\wotb_mod_api.h
echo Example: build\hello_mod.dll
echo V3 Core: build\v3_core_runtime_tests.exe
echo V3 Svcs: build\v3_runtime_services_tests.exe
echo V3 Safe: build\v3_gameplay_bridge_tests.exe
echo API QA:  build\api_full_host.exe
echo Publish: build\published_selftest_host.exe
echo New API: build\new_api_mods_host.exe
echo V3 Mods: build\v3_core_runtime_test_mod.dll + build\v3_client_resources_test_mod.dll
echo Live QA: build\native_validation_host_tests.exe + build\native_validation_mod.dll
echo GES services: build\v3_ges_services_tests.exe
echo GES native: build\v3_ges_native_tests.exe
echo Hook observers: build\v3_hook_observers_tests.exe
echo Lua Host: build\lua_host_packages\wotbmod.lua_host.wotbmod
echo Lua Demos: build\lua_host_distribution\mods\lua\example.lua_hello + example.lua_ui_framework
popd
exit /b 0

:failed
echo.
echo Build, smoke, or full contract test failed.
popd
exit /b 1
