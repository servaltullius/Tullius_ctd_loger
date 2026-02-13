@echo off
setlocal

set "SRC=%~dp0.."
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"
set "RID=win-x64"
set "TFM=net8.0-windows10.0.19041.0"
set "BUILD_OUT=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%"
set "BUILD_OUT_X64=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  exit /b 2
)

rem Lightweight build (framework-dependent, unpackaged WinUI):
rem - Uses the WinUI build output that includes required runtime assets (.pri/.xbf)
rem - Requires user machine runtimes (.NET Desktop Runtime 8 + Windows App Runtime)
dotnet build "%PROJECT%" -c Release -r %RID%
if errorlevel 1 exit /b 1

rem Check both possible output paths (with/without platform subfolder)
if exist "%BUILD_OUT_X64%\SkyrimDiagDumpToolWinUI.exe" set "BUILD_OUT=%BUILD_OUT_X64%"
if not exist "%BUILD_OUT%\SkyrimDiagDumpToolWinUI.exe" (
  echo ERROR: expected WinUI build output not found: %BUILD_OUT%
  exit /b 3
)

if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

robocopy "%BUILD_OUT%" "%OUT%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1

echo WinUI build output: %OUT%
