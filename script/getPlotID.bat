@echo off
setlocal enabledelayedexpansion

set address=11634x35VB6NDyC9BnMtuhqoDVcMMnaHNi

cd %~dp0
for /f %%a in ('parallel-plotter.exe --from_addr %address%') do ( set pid=%%a)
echo %pid%
pause