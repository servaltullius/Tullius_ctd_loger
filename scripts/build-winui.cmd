@echo off
setlocal

set "SRC=%~dp0.."
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  exit /b 2
)

dotnet publish "%PROJECT%" -c Release -r win-x64 --self-contained true ^
  -p:WindowsAppSDKSelfContained=true ^
  -p:WindowsAppSdkUndockedRegFreeWinRTInitialize=true ^
  -o "%OUT%"
if errorlevel 1 exit /b 1

echo WinUI publish output: %OUT%
