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

if not exist "build\lua54.lib" (
    echo build\lua54.lib is missing; run tests\build_lua_vendor_tests.cmd first.
    goto :failed
)
if not exist "build\lua_host" mkdir "build\lua_host"
if not exist "build\lua_convert" mkdir "build\lua_convert"
if not exist "build\lua_host_tests" mkdir "build\lua_host_tests"
if not exist "build\lua_host_release" mkdir "build\lua_host_release"
if not exist "build\lua_dava_stub" mkdir "build\lua_dava_stub"

python tools\generate_lua_bindings.py --output "build\lua_generated_bindings.cpp" --report
if not "%errorlevel%"=="0" goto :failed

rem The model's own tests, run here because this is the only script that runs
rem the generator. They check the things the C++ build cannot see: that the
rem slot inventory is still 589, that every ABI constant lands on exactly one
rem wotb.* table under the spelling shipped scripts already use, and that no
rem table publishes one name twice. A duplicate name compiles perfectly - the
rem second assignment silently wins and the first constant simply is not there
rem any more - so the compiler is the wrong place to catch it.
python -B tests\test_lua_api_model.py
if not "%errorlevel%"=="0" goto :failed

rem A DLL with the production loader's base name makes the private bridge
rem discoverable through GetModuleHandleW/GetProcAddress. Its callbacks are
rem owner- and kind-checking fakes, so the Lua surface is exercised end to end
rem without linking the real game loader or exposing provider pointers.
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude /I. ^
    tests\lua_dava_bridge_stub.cpp ^
    /Fo"build\lua_dava_stub\\" ^
    /link /OUT:"build\lua_dava_stub\wotb_mod_loader.dll" ^
    /IMPLIB:"build\lua_dava_stub\wotb_mod_loader.lib"
if not "%errorlevel%"=="0" goto :failed

rem /DWOTBMOD_LUA_HOST_TESTS is what compiles the *ForTests exports in. Three of
rem them mint scripts at the host's full ceiling, so they are gated out of a
rem release DLL; the release build below proves they really are gone rather than
rem merely intended to be.
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /DWOTBMOD_LUA_HOST_TESTS ^
    /Iinclude /I. ^
    loader\lua\lua_host_mod.cpp ^
    loader\lua\lua_dev_files.cpp ^
    loader\lua\lua_script.cpp ^
    loader\lua\lua_convert.cpp ^
    loader\lua\lua_bindings.cpp ^
    loader\lua\lua_preludes.cpp ^
    loader\lua\lua_ownership.cpp ^
    loader\lua\lua_permission_runtime.cpp ^
    loader\lua\lua_manifest_scanner.cpp ^
    loader\lua\lua_bind_storage.cpp ^
    loader\lua\lua_bind_events.cpp ^
    loader\lua\lua_bind_ges.cpp ^
    loader\lua\lua_bind_ui.cpp ^
    build\lua_generated_bindings.cpp ^
    loader\lua\lua_watcher.cpp ^
    "build\lua54.lib" ^
    user32.lib ^
    /Fo"build\lua_host\\" ^
    /link /OUT:"build\wotbmod_lua_host.dll" ^
    /IMPLIB:"build\wotbmod_lua_host.lib"
if not "%errorlevel%"=="0" goto :failed

cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude /I. ^
    tests\lua_host_tests.cpp ^
    loader\lua\lua_watcher.cpp ^
    /Fo"build\lua_host_tests\\" ^
    /link /OUT:"build\lua_host_tests.exe"
if not "%errorlevel%"=="0" goto :failed

rem Not `if errorlevel 1`: that reads as "errorlevel >= 1", and a process that
rem dies on an access violation exits with 0xC0000005, which cmd holds as the
rem signed value -1073741819. A crashing test therefore walks straight past
rem `if errorlevel 1` and the script prints its success banner. Demonstrated
rem here, not theorised: a deliberately corrupted Lua stack segfaulted
rem lua_convert_tests.exe and this script still said "Lua host contract
rem passed". A string compare against 0 catches every non-zero exit.
"build\lua_host_tests.exe" "build\wotbmod_lua_host.dll" ^
    "examples\lua_host\manifest.json" ^
    "build\lua_dava_stub\wotb_mod_loader.dll"
if not "%errorlevel%"=="0" goto :failed

rem The conversion rules are proven against a real lua_State rather than
rem through the host DLL: they are pure functions over a stack, and every
rem later binding is generated on top of them.
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- ^
    /Iinclude ^
    tests\lua_convert_tests.cpp ^
    loader\lua\lua_convert.cpp ^
    "build\lua54.lib" ^
    /Fo"build\lua_convert\\" ^
    /link /OUT:"build\lua_convert_tests.exe"
if not "%errorlevel%"=="0" goto :failed

"build\lua_convert_tests.exe"
if not "%errorlevel%"=="0" goto :failed

rem ---------------------------------------------------------------------------
rem The release DLL: the same sources without WOTBMOD_LUA_HOST_TESTS.
rem
rem Two things are checked, and the first is the one that would otherwise rot.
rem A build flag that removes exports is only a security gate while somebody
rem verifies the exports are removed, so the export table is read and any name
rem ending in ForTests fails the build. Three of those entry points mint scripts
rem at the host's full permission ceiling; they must not exist in a shipped DLL.
rem
rem The second is that the release configuration still compiles at all. Code
rem inside an #ifdef that nothing defines is code nobody type-checks, and the
rem reverse is just as true - it is easy to leave a helper used only by the
rem gated block and break the build for everyone who does not define the flag.
cl /nologo /std:c++17 /O2 /MD /EHsc /W4 /WX /permissive- /LD ^
    /Iinclude /I. ^
    loader\lua\lua_host_mod.cpp ^
    loader\lua\lua_dev_files.cpp ^
    loader\lua\lua_script.cpp ^
    loader\lua\lua_convert.cpp ^
    loader\lua\lua_bindings.cpp ^
    loader\lua\lua_preludes.cpp ^
    loader\lua\lua_ownership.cpp ^
    loader\lua\lua_permission_runtime.cpp ^
    loader\lua\lua_manifest_scanner.cpp ^
    loader\lua\lua_bind_storage.cpp ^
    loader\lua\lua_bind_events.cpp ^
    loader\lua\lua_bind_ges.cpp ^
    loader\lua\lua_bind_ui.cpp ^
    build\lua_generated_bindings.cpp ^
    loader\lua\lua_watcher.cpp ^
    "build\lua54.lib" ^
    user32.lib ^
    /Fo"build\lua_host_release\\" ^
    /link /OUT:"build\lua_host_release\wotbmod_lua_host.dll" ^
    /IMPLIB:"build\lua_host_release\wotbmod_lua_host.lib"
if not "%errorlevel%"=="0" goto :failed

rem findstr exits 0 when it finds something, so finding a ForTests export is the
rem failure. WotbModLoadV3 is checked in the same pass: a DLL that exported
rem nothing at all would trivially pass the first check and be useless.
dumpbin /nologo /exports "build\lua_host_release\wotbmod_lua_host.dll" > "build\lua_host_release\exports.txt"
if not "%errorlevel%"=="0" goto :failed
findstr /C:"ForTests" "build\lua_host_release\exports.txt" >nul
if "%errorlevel%"=="0" (
    echo The release DLL still exports test-only entry points:
    findstr /C:"ForTests" "build\lua_host_release\exports.txt"
    goto :failed
)
findstr /C:"WotbModLoadV3" "build\lua_host_release\exports.txt" >nul
if not "%errorlevel%"=="0" (
    echo The release DLL does not export WotbModLoadV3.
    goto :failed
)
echo Release DLL exports WotbModLoadV3 and no test entry points.

echo Lua host contract passed.
popd
exit /b 0

:failed
echo Lua host contract failed.
popd
exit /b 1
