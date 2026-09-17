@echo off
rem ---------------------------------------------------------------------------
rem  vsdev.cmd - run a command inside the Visual Studio 2022 x64 developer
rem  environment.  Used by the build helpers and by CI-like local scripts:
rem     scripts\vsdev.cmd cmake --build --preset windows-msvc-cuda-release
rem
rem  Resolution order for VsDevCmd.bat:
rem    1. VSDEVCMD environment variable (explicit override)
rem    2. the well-known 2022 edition folders (Enterprise, Professional,
rem       Community, BuildTools, Preview)
rem  vswhere.exe is deliberately not used: its folder name contains "(x86)"
rem  which breaks cmd's FOR /F parser, and not every machine ships it.
rem ---------------------------------------------------------------------------
setlocal
set "DEVCMD="
if defined VSDEVCMD if exist "%VSDEVCMD%" set "DEVCMD=%VSDEVCMD%"

if not defined DEVCMD (
  for %%e in (Enterprise Professional Community BuildTools Preview) do (
    if not defined DEVCMD if exist "C:\Program Files\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat" set "DEVCMD=C:\Program Files\Microsoft Visual Studio\2022\%%e\Common7\Tools\VsDevCmd.bat"
  )
)

if not defined DEVCMD (
  echo vsdev.cmd: Visual Studio 2022 with the C++ toolset was not found 1>&2
  exit /b 1
)

call "%DEVCMD%" -arch=x64 -host_arch=x64 -no_logo >nul
if not defined VCPKG_ROOT set "VCPKG_ROOT=C:\vcpkg"
%*
exit /b %ERRORLEVEL%
