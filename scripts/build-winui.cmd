@echo off
setlocal

rem Resolve SRC to an absolute path (handles UNC paths and trailing ..)
for %%I in ("%~dp0..") do set "SRC=%%~fI"
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"
set "RID=win-x64"
set "TFM=net8.0-windows10.0.19041.0"
set "BUILD_OUT=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%"
set "BUILD_OUT_X64=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%"
set "BUILD_OUT_BASE=%SRC%\dump_tool_winui\bin\Release\%TFM%"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  exit /b 2
)

rem Framework-dependent build (unpackaged WinUI):
rem - User must install .NET 8 Desktop Runtime + Windows App Runtime
rem - Much smaller output (~29 MB zipped vs ~57 MB self-contained)
dotnet build "%PROJECT%" -c Release -r %RID%
if errorlevel 1 exit /b 1

set "FINAL_OUT="

rem Prefer output that includes required WinUI XAML artifacts.
call :set_candidate "%BUILD_OUT%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_BASE%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_X64%"
if defined FINAL_OUT goto :copy_output

echo ERROR: expected WinUI output not found ^(or missing App.xbf/MainWindow.xbf/.pri^)
echo Tried:
echo   %BUILD_OUT%
echo   %BUILD_OUT_BASE%
echo   %BUILD_OUT_X64%
exit /b 3

:set_candidate
set "CAND=%~1"
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.exe" goto :eof
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.pri" goto :eof
if not exist "%CAND%\App.xbf" goto :eof
if not exist "%CAND%\MainWindow.xbf" goto :eof
set "FINAL_OUT=%CAND%"
goto :eof

:copy_output
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

robocopy "%FINAL_OUT%" "%OUT%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1

echo WinUI build output: %OUT%
exit /b 0
