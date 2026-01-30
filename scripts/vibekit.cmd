@echo off
REM Convenience wrapper for Windows (Git Bash / cmd / PowerShell).
REM Uses the Python launcher (py) when available.
if exist "%~dp0vibekit.py" (
  py -3 "%~dp0vibekit.py" %*
  exit /b %ERRORLEVEL%
)
echo vibekit.py not found.
exit /b 1

