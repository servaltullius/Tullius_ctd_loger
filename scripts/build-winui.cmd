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
set "STAGING_OUT=%SRC%\build-winui.next"
set "PREVIOUS_OUT=%SRC%\build-winui.previous"
set "RID=win-x64"
set "TFM=net8.0-windows10.0.19041.0"
set "WINUI_PLATFORM=x64"
set "XAML_BUILD_ROOT=%SRC%\dump_tool_winui\bin\%WINUI_PLATFORM%\Release"
set "XAML_INTERMEDIATE_ROOT=%SRC%\dump_tool_winui\obj\%WINUI_PLATFORM%\Release"
set "XAML_BUILD_OUT=%XAML_BUILD_ROOT%\%TFM%\%RID%"
set "WINUI_MANIFEST=%SRC%\dump_tool_winui\obj\SkyrimDiagVersion\app.manifest"
set "PROJECT_VERSION="
set "INFORMATIONAL_VERSION="

for /f "tokens=1,2 delims=|" %%V in ('python -c "import sys; sys.path.insert(0, 'scripts'); from release_contract import project_version, source_state; v=project_version('.'); s=source_state('.'); print(v + '|' + v + '+' + str(s['git_commit']) + ('.dirty' if s['git_dirty'] else ''))"') do (
  set "PROJECT_VERSION=%%V"
  set "INFORMATIONAL_VERSION=%%W"
)
if not defined PROJECT_VERSION (
  echo ERROR: failed to resolve project version from CMakeLists.txt
  set "EXITCODE=4"
  goto :cleanup
)
if not defined INFORMATIONAL_VERSION (
  echo ERROR: failed to resolve Git provenance for WinUI version metadata
  set "EXITCODE=4"
  goto :cleanup
)

if not exist "%PROJECT%" (
  echo ERROR: WinUI project not found: %PROJECT%
  set "EXITCODE=2"
  goto :cleanup
)

python "%SRC%\scripts\generate_winui_manifest.py" ^
  --repo-root "%SRC%" ^
  --template "%SRC%\dump_tool_winui\app.manifest.in" ^
  --output "%WINUI_MANIFEST%"
if errorlevel 1 (
  set "EXITCODE=1"
  goto :cleanup
)

rem Self-contained unpackaged WinUI publish:
rem - Bundles .NET and Windows App SDK files next to the exe for MO2 zip deployment.
rem - Larger output, but avoids fragile end-user Windows App Runtime side-by-side installs.
rem - Publishes into one freshly deleted staging directory, so an old complete
rem   candidate can never be selected instead of this invocation's output.
if exist "%STAGING_OUT%" rmdir /s /q "%STAGING_OUT%"
if exist "%STAGING_OUT%" (
  echo ERROR: failed to clear WinUI staging output: %STAGING_OUT%
  set "EXITCODE=1"
  goto :cleanup
)
if exist "%XAML_BUILD_ROOT%" rmdir /s /q "%XAML_BUILD_ROOT%"
if exist "%XAML_INTERMEDIATE_ROOT%" rmdir /s /q "%XAML_INTERMEDIATE_ROOT%"
if exist "%XAML_BUILD_ROOT%" (
  echo ERROR: failed to clear WinUI Release build output
  set "EXITCODE=1"
  goto :cleanup
)
if exist "%XAML_INTERMEDIATE_ROOT%" (
  echo ERROR: failed to clear WinUI Release intermediate output
  set "EXITCODE=1"
  goto :cleanup
)

dotnet publish "%PROJECT%" -c Release -r %RID% --self-contained true --output "%STAGING_OUT%" ^
  -p:Platform=%WINUI_PLATFORM% ^
  -p:WindowsAppSDKSelfContained=true ^
  -p:PublishSingleFile=false ^
  -p:PublishTrimmed=false ^
  -p:SkyrimDiagVersion=%PROJECT_VERSION% ^
  -p:SkyrimDiagInformationalVersion=%INFORMATIONAL_VERSION% ^
  "-p:SkyrimDiagApplicationManifest=%WINUI_MANIFEST%"
if errorlevel 1 (
  set "EXITCODE=1"
  goto :cleanup
)

for %%F in (App.xbf MainWindow.xbf SkyrimDiagDumpToolWinUI.pri) do (
  if not exist "%XAML_BUILD_OUT%\%%F" (
    echo ERROR: fresh WinUI XAML asset missing: %XAML_BUILD_OUT%\%%F
    set "EXITCODE=3"
    goto :cleanup
  )
  copy /Y "%XAML_BUILD_OUT%\%%F" "%STAGING_OUT%\%%F" >nul
  if errorlevel 1 (
    echo ERROR: failed to stage fresh WinUI XAML asset: %%F
    set "EXITCODE=1"
    goto :cleanup
  )
)

call :validate_output "%STAGING_OUT%"
if errorlevel 1 (
  echo ERROR: fresh WinUI staging output is incomplete: %STAGING_OUT%
  set "EXITCODE=3"
  goto :cleanup
)

python "%SRC%\scripts\write_build_provenance.py" ^
  --repo-root "%SRC%" ^
  --output "%STAGING_OUT%\SkyrimDiagBuildProvenance.json" ^
  --kind winui ^
  --configuration Release ^
  --artifact-root "%STAGING_OUT%"
if errorlevel 1 (
  set "EXITCODE=1"
  goto :cleanup
)

if exist "%PREVIOUS_OUT%" rmdir /s /q "%PREVIOUS_OUT%"
if exist "%PREVIOUS_OUT%" (
  echo ERROR: failed to clear previous WinUI output: %PREVIOUS_OUT%
  set "EXITCODE=1"
  goto :cleanup
)

if exist "%OUT%" (
  move "%OUT%" "%PREVIOUS_OUT%" >nul
  if errorlevel 1 (
    echo ERROR: failed to stage existing WinUI output for replacement
    set "EXITCODE=1"
    goto :cleanup
  )
)

move "%STAGING_OUT%" "%OUT%" >nul
if errorlevel 1 (
  echo ERROR: failed to install fresh WinUI output
  if exist "%PREVIOUS_OUT%" move "%PREVIOUS_OUT%" "%OUT%" >nul
  set "EXITCODE=1"
  goto :cleanup
)
if exist "%PREVIOUS_OUT%" rmdir /s /q "%PREVIOUS_OUT%"

echo WinUI build output: %OUT%
goto :cleanup

:validate_output
set "CAND=%~1"
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.exe" exit /b 1
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.pri" exit /b 1
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.runtimeconfig.json" exit /b 1
if not exist "%CAND%\SkyrimDiagDumpToolWinUI.deps.json" exit /b 1
if not exist "%CAND%\App.xbf" exit /b 1
if not exist "%CAND%\MainWindow.xbf" exit /b 1
if not exist "%CAND%\Microsoft.WindowsAppRuntime.Bootstrap.dll" exit /b 1
if not exist "%CAND%\Microsoft.WindowsAppRuntime.dll" exit /b 1
if not exist "%CAND%\Microsoft.WindowsAppRuntime.pri" exit /b 1
if not exist "%CAND%\Microsoft.ui.xaml.dll" exit /b 1
if not exist "%CAND%\Microsoft.UI.pri" exit /b 1
if not exist "%CAND%\CoreMessagingXP.dll" exit /b 1
exit /b 0

:cleanup
popd >nul
exit /b %EXITCODE%
