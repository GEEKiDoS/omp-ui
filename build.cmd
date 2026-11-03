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
rem cl prints its /showIncludes prefix in the console code page, while the prefix ninja matches is
rem in the ANSI code page. From a console in another code page (e.g. UTF-8, 65001) ninja records no
rem header dependencies, and header edits then leave objects built against the old header.
for /f "tokens=3" %%c in ('reg query HKLM\SYSTEM\CurrentControlSet\Control\Nls\CodePage /v ACP') do set "ACP=%%c"
for /f "tokens=2 delims=:." %%c in ('chcp') do set /a "CONSOLECP=%%c"
chcp %ACP% >nul
cmake -S "%~dp0." -B "%~dp0build" -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build "%~dp0build" %*
set "STATUS=%ERRORLEVEL%"
chcp %CONSOLECP% >nul
exit /b %STATUS%
