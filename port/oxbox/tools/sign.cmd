@echo off
rem sign.cmd: drop Halo 1 Unlimited packages (.oxpk) on this to check them and sign them with the key
rem "oxsign.py setup" chose (port/oxbox/installer/README.md, Signing releases)
py "%~dp0oxsign.py" open %*
if errorlevel 9009 python "%~dp0oxsign.py" open %*
