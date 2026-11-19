@echo off
rem OpenGOAL Switch Build App - double-click launcher (Windows)
cd /d "%~dp0"
where python >nul 2>nul
if %errorlevel%==0 (
  python scripts\switch\extractor_app.py
) else (
  py -3 scripts\switch\extractor_app.py
)
pause
