@echo off
setlocal

set "SRC=%~dp0.."
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  exit /b 2
)

rem Lightweight publish (framework-dependent):
rem - Smaller package size
rem - Requires user machine runtimes (.NET Desktop Runtime 8 + Windows App Runtime)
dotnet publish "%PROJECT%" -c Release -r win-x64 --self-contained false ^
  -p:WindowsAppSDKSelfContained=false ^
  -o "%OUT%"
if errorlevel 1 exit /b 1

echo WinUI publish output: %OUT%
