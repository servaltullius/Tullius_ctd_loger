@echo off
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if exist "%VSWHERE%" (
  for /f "usebackq tokens=* delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSROOT=%%I"
  )
)

if not defined VSROOT set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
if not exist "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" (
  echo ERROR: vcvars64.bat not found under "%VSROOT%"
  exit /b 2
)

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

if not defined VCPKG_ROOT set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"
if not exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
  echo ERROR: vcpkg toolchain not found: "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
  exit /b 3
)

set "SRC=%~dp0.."
set "BUILD=%SRC%\build-win"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"
if not exist "%NINJA%" set "NINJA=ninja"

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%BUILD%"
if errorlevel 1 exit /b 1

echo Built successfully: %BUILD%
