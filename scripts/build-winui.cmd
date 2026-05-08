@echo off
setlocal
set "EXITCODE=0"

pushd "%~dp0.." >nul 2>&1
if errorlevel 1 (
  echo ERROR: failed to enter repo root from "%~dp0.."
  exit /b 4
)

set "SRC=%CD%"
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"
set "RID=win-x64"
set "TFM=net8.0-windows10.0.19041.0"
set "BUILD_OUT=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%\publish"
set "BUILD_OUT_RID=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%"
set "BUILD_OUT_X64=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%\publish"
set "BUILD_OUT_X64_RID=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%"
set "BUILD_OUT_BASE=%SRC%\dump_tool_winui\bin\Release\%TFM%"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  set "EXITCODE=2"
  goto :cleanup
)

rem Self-contained unpackaged WinUI publish:
rem - Bundles .NET and Windows App SDK files next to the exe for MO2 zip deployment.
rem - Larger output, but avoids fragile end-user Windows App Runtime side-by-side installs.
dotnet publish "%PROJECT%" -c Release -r %RID% --self-contained true ^
  -p:WindowsAppSDKSelfContained=true ^
  -p:PublishSingleFile=false ^
  -p:PublishTrimmed=false
if errorlevel 1 (
  set "EXITCODE=1"
  goto :cleanup
)

set "FINAL_OUT="

rem Prefer output that includes the full WinUI runtime asset set used by packaging/release gate.
call :set_candidate "%BUILD_OUT%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_RID%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_BASE%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_X64%"
if defined FINAL_OUT goto :copy_output

call :set_candidate "%BUILD_OUT_X64_RID%"
if defined FINAL_OUT goto :copy_output

echo ERROR: expected WinUI output not found ^(or missing required runtime assets^)
echo Tried:
echo   %BUILD_OUT%
echo   %BUILD_OUT_RID%
echo   %BUILD_OUT_BASE%
echo   %BUILD_OUT_X64%
echo   %BUILD_OUT_X64_RID%
set "EXITCODE=3"
goto :cleanup

:set_candidate
set "CAND=%~1"
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.exe" goto :eof
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.pri" goto :eof
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.runtimeconfig.json" goto :eof
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.deps.json" goto :eof
if not exist "%CAND%\App.xbf" goto :eof
if not exist "%CAND%\MainWindow.xbf" goto :eof
set "FINAL_OUT=%CAND%"
goto :eof

:copy_output
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

robocopy "%FINAL_OUT%" "%OUT%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 (
  set "EXITCODE=1"
  goto :cleanup
)

echo WinUI build output: %OUT%
goto :cleanup

:cleanup
popd >nul
exit /b %EXITCODE%
