@echo off
setlocal
pushd "%~dp0\.."

python -B tests\test_wotbmod_cli.py
if not "%errorlevel%"=="0" goto :failed

rem The generated reference, the schemas, the Lua templates and the snippets
rem stay in step with the code (tests/test_sdk_docs.py).
python -B tests\test_sdk_docs.py
if not "%errorlevel%"=="0" goto :failed

rem Stage 3: install/uninstall/update/rollback/release/publish and the
rem pure-Python P-256 verifier (tests/test_wotbmod_packages.py).
python -B tests\test_wotbmod_packages.py
if not "%errorlevel%"=="0" goto :failed

rem The portal lives in ..\wotbmod-portal; its end-to-end suite publishes with this CLI,
rem and its vendored copies of the SDK modules must not lag behind tools\.
python -B ..\wotbmod-portal\tools\sync_sdk.py --check
if not "%errorlevel%"=="0" goto :failed
python -B ..\wotbmod-portal\tests\test_portal.py
if not "%errorlevel%"=="0" goto :failed

rem Stage 7: the static scanner and its gates (tests/test_wotbmod_scan.py).
python -B tests\test_wotbmod_scan.py
if not "%errorlevel%"=="0" goto :failed

rem Stage 5: the wotbmod:// launcher and Lua packages (tests/test_wotbmod_launcher.py).
python -B tests\test_wotbmod_launcher.py
if not "%errorlevel%"=="0" goto :failed
rem The frozen wotbmod.exe entry (tests/test_wotbmod_entry.py).
python -B tests\test_wotbmod_entry.py
if not "%errorlevel%"=="0" goto :failed

echo === WotbMod developer CLI tests passed ===
popd
exit /b 0

:failed
echo WotbMod developer CLI tests failed.
popd
exit /b 1
