@echo off
REM Convenience wrapper for Windows (cmd/PowerShell).
REM Uses the Python launcher (py) when available.
if exist "%~dp0vibe.py" (
  py -3 "%~dp0vibe.py" %*
  exit /b %ERRORLEVEL%
)
echo vibe.py not found.
exit /b 1

