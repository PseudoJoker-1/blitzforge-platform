@echo off
REM Build version.dll proxy (32-bit) for World of Tanks Blitz
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat" >nul
if not "%errorlevel%"=="0" ( echo vcvars32 failed & exit /b 1 )

pushd "%~dp0"

REM Compile + link as version.dll using forwarding .def
cl /nologo /O2 /MD /LD /EHsc /W3 ^
   /Ithird_party\minhook\include /Ithird_party\minhook\src\hde ^
   dllmain.cpp ^
   third_party\minhook\src\buffer.c ^
   third_party\minhook\src\hook.c ^
   third_party\minhook\src\trampoline.c ^
   third_party\minhook\src\hde\hde32.c ^
   /link /OUT:version.dll /SUBSYSTEM:WINDOWS ^
   /MACHINE:X86 user32.lib kernel32.lib gdi32.lib d3d11.lib dxgi.lib d3dcompiler.lib dbghelp.lib

if not "%errorlevel%"=="0" ( echo Build failed & popd & exit /b 1 )

del /q version.exp version.lib *.obj 2>nul
echo.
echo === Built version.dll ===
dir version.dll | findstr version

popd
endlocal
