@echo off
title Backport Helper
cd /d "%~dp0tools"
where py >nul 2>nul && (py -3 backport_helper.py) || (python backport_helper.py)
if errorlevel 1 pause
