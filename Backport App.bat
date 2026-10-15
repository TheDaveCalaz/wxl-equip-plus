@echo off
title Backport App (test phase) - keep this window open
cd /d "%~dp0tools\app"
where py >nul 2>nul && (py -3 server.py) || (python server.py)
if errorlevel 1 pause
