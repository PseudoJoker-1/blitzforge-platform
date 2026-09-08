@echo off
rem `wotbmod` from any terminal: the frozen wotbmod.exe when the bundle shipped
rem it, otherwise the Python tools next to this file.
if exist "%~dp0wotbmod.exe" (
    "%~dp0wotbmod.exe" %*
) else (
    python "%~dp0wotbmod.py" %*
)
