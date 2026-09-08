@echo off
setlocal
set "WOTBMOD_V3_HTTP_LIVE_TEST=1"
call "%~dp0build_v3_http_transport_tests.cmd"
exit /b %errorlevel%
