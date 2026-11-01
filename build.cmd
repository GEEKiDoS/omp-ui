@echo off
rem Configure and build with native MSVC + Ninja. Usage: build.cmd [--target X]
setlocal
set "COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
set "VCVARS=vcvars64.bat"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
	set "COMPONENT=Microsoft.VisualStudio.Component.VC.Tools.ARM64"
	set "VCVARS=vcvarsarm64.bat"
)
set "VSROOT="
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires %COMPONENT% -property installationPath`) do set "VSROOT=%%i"
if not defined VSROOT (
	echo Visual Studio with the native C++ tools was not found.
	exit /b 1
)
call "%VSROOT%\VC\Auxiliary\Build\%VCVARS%" >nul || exit /b 1
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release || exit /b 1
cmake --build "%~dp0build" %* || exit /b 1
