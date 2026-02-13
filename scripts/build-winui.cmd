@echo off
setlocal

rem Resolve SRC to an absolute path (handles UNC paths and trailing ..)
for %%I in ("%~dp0..") do set "SRC=%%~fI"
set "PROJECT=%SRC%\dump_tool_winui\SkyrimDiagDumpToolWinUI.csproj"
set "OUT=%SRC%\build-winui"
set "RID=win-x64"
set "TFM=net8.0-windows10.0.19041.0"
set "PUBLISH_OUT=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%\publish"
set "PUBLISH_OUT_X64=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%\publish"
set "BUILD_OUT=%SRC%\dump_tool_winui\bin\Release\%TFM%\%RID%"
set "BUILD_OUT_X64=%SRC%\dump_tool_winui\bin\x64\Release\%TFM%\%RID%"

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  exit /b 2
)

rem Self-contained publish (unpackaged WinUI):
rem - SelfContained + WindowsAppSdkSelfContained passed via CLI (not .csproj)
rem   to avoid breaking WSL/Linux typecheck builds where mt.exe can't handle UNC paths
rem - No .NET Desktop Runtime or Windows App Runtime install required on user machine
rem - xcopy-deployable folder
dotnet publish "%PROJECT%" -c Release -r %RID% -p:SelfContained=true -p:WindowsAppSdkSelfContained=true
if errorlevel 1 (
  echo WARNING: dotnet publish failed, falling back to dotnet build...
  dotnet build "%PROJECT%" -c Release -r %RID%
  if errorlevel 1 exit /b 1
  goto :find_build_output
)

rem Check publish output paths (with/without platform subfolder)
if exist "%PUBLISH_OUT_X64%\SkyrimDiagDumpToolWinUI.exe" (
  set "FINAL_OUT=%PUBLISH_OUT_X64%"
  goto :copy_output
)
if exist "%PUBLISH_OUT%\SkyrimDiagDumpToolWinUI.exe" (
  set "FINAL_OUT=%PUBLISH_OUT%"
  goto :copy_output
)

:find_build_output
rem Fallback: check build output (non-publish) paths
if exist "%BUILD_OUT_X64%\SkyrimDiagDumpToolWinUI.exe" (
  set "FINAL_OUT=%BUILD_OUT_X64%"
  goto :copy_output
)
if exist "%BUILD_OUT%\SkyrimDiagDumpToolWinUI.exe" (
  set "FINAL_OUT=%BUILD_OUT%"
  goto :copy_output
)

echo ERROR: expected WinUI output not found
exit /b 3

:copy_output
if exist "%OUT%" rmdir /s /q "%OUT%"
mkdir "%OUT%"

robocopy "%FINAL_OUT%" "%OUT%" /E /NFL /NDL /NJH /NJS /NP >nul
if errorlevel 8 exit /b 1

rem Remove clearly unnecessary Windows App SDK satellite DLLs to reduce package
rem size.  These are auto-included by the SDK but never used by SkyrimDiag.
rem (Safe to delete: AI, ML, OAuth, WebView2, OnnxRuntime, etc.)
for %%F in (
  "Microsoft.ML.OnnxRuntime.dll"
  "Microsoft.Security.Authentication.OAuth.Projection.dll"
  "Microsoft.Web.WebView2.Core.dll"
  "Microsoft.Web.WebView2.Core.Projection.dll"
  "Microsoft.Windows.AppNotifications.Builder.Projection.dll"
  "Microsoft.Graphics.Imaging.Projection.dll"
  "Microsoft.InteractiveExperiences.Projection.dll"
  "onnxruntime.dll"
  "onnxruntime_providers_shared.dll"
  "DirectML.dll"
  "Microsoft.DiaSymReader.Native.amd64.dll"
  "WebView2Loader.dll"
  "Microsoft.Windows.Widgets.dll"
  "Microsoft.Windows.Widgets.Projection.dll"
  "Microsoft.Windows.Workloads.dll"
  "Microsoft.Windows.Workloads.Resources.dll"
  "Microsoft.Windows.Workloads.Resources_ec.dll"
  "SessionHandleIPCProxyStub.dll"
  "PushNotificationsLongRunningTask.ProxyStub.dll"
  "WindowsAppSdk.AppxDeploymentExtensions.Desktop.dll"
  "WindowsAppSdk.AppxDeploymentExtensions.Desktop-EventLog-Instrumentation.dll"
  "WindowsAppRuntime.DeploymentExtensions.OneCore.dll"
  "Microsoft.WindowsAppRuntime.Insights.Resource.dll"
) do (
  if exist "%OUT%\%%~F" del /q "%OUT%\%%~F" 2>nul
)
rem Wildcard patterns need a for /r loop
for /r "%OUT%" %%F in (Microsoft.Windows.AI.*.dll) do del /q "%%F" 2>nul
rem Remove AI/ML/Vision/SemanticSearch winmd metadata files
for /r "%OUT%" %%F in (Microsoft.Windows.AI.*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Windows.Vision*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Windows.SemanticSearch*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Windows.Private*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Graphics.Imaging*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Graphics.ImagingInternal*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (Microsoft.Windows.Globalization*.winmd) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (workloads*.json) do del /q "%%F" 2>nul
for /r "%OUT%" %%F in (workloads*.stx.json) do del /q "%%F" 2>nul

echo WinUI build output: %OUT%
exit /b 0
