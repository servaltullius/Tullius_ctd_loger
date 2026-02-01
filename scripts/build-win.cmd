@echo off
setlocal

set "VSROOT=C:\Program Files\Microsoft Visual Studio\2022\Community"
set "VCPKG_ROOT=%VSROOT%\VC\vcpkg"

call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat"
if errorlevel 1 exit /b 1

set "SRC=%~dp0.."
set "BUILD=%SRC%\build-win"
set "CMAKE=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "NINJA=%VSROOT%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"

"%CMAKE%" -S "%SRC%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_MAKE_PROGRAM="%NINJA%" ^
  -DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
if errorlevel 1 exit /b 1

"%CMAKE%" --build "%BUILD%"
if errorlevel 1 exit /b 1

echo Built successfully: %BUILD%

