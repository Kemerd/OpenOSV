@echo off
rem Disposable: compile a research probe against the main checkout's release
rem build, into research\aistitch\bin.  Run through scripts\vsdev.cmd:
rem     scripts\vsdev.cmd research\aistitch\build_probe.cmd            (bandprobe: per-lens linear RGB bands)
rem     scripts\vsdev.cmd research\aistitch\build_probe.cmd seamprobe  (seamprobe: carved seam per frame / bucket)
setlocal
set "B=L:\Dev\premiere_360_reframe\build\windows-msvc-premiere-release"
set "V=L:\Dev\premiere_360_reframe\vcpkg_installed\x64-windows"
set "CU=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
set "OUT=%~dp0bin"
rem %1 = bandprobe (default) or seamprobe
set "NAME=%~1"
if "%NAME%"=="" set "NAME=bandprobe"
set "SRC=%~dp0..\neural\bandprobe.cpp"
if /I "%NAME%"=="seamprobe" set "SRC=%~dp0seamprobe.cpp"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /O2 /MD -std:c++20 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /bigobj ^
  -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DCLI11_COMPILE ^
  -DOSV_HAVE_COLOR=1 -DOSV_HAVE_CONTAINER=1 -DOSV_HAVE_CUDA=1 -DOSV_HAVE_GEOM=1 -DOSV_HAVE_IO=1 ^
  -DOSV_HAVE_META=1 -DOSV_HAVE_OPENCL=1 -DOSV_HAVE_RENDER=1 -DOSV_HAVE_VIDEO=1 -DOSV_VIDEO_HAVE_CUDA=1 ^
  -DCL_HPP_MINIMUM_OPENCL_VERSION=120 -DCL_HPP_TARGET_OPENCL_VERSION=120 -DCL_TARGET_OPENCL_VERSION=120 ^
  -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS ^
  -IL:\Dev\premiere_360_reframe\include -I"%B%\generated" -I"%V%\include" -I"%CU%\include" ^
  "%SRC%" ^
  "%B%\tools\osvtool\CMakeFiles\osvtool.dir\Pipeline.cpp.obj" ^
  /Fo"%OUT%\\" /Fe"%OUT%\%NAME%.exe" ^
  /link /LIBPATH:"%V%\lib" /LIBPATH:"%CU%\lib\x64" ^
  "%B%\lib\osv_io.lib" "%B%\lib\osv_render.lib" "%B%\lib\osv_render_cuda.lib" "%B%\lib\osv_render_opencl.lib" ^
  "%B%\lib\osv_render_cpu.lib" "%B%\lib\osv_video.lib" "%B%\lib\osv_geom.lib" "%B%\lib\osv_color.lib" ^
  "%B%\lib\osv_meta.lib" "%B%\lib\osv_container.lib" "%B%\lib\osv_core.lib" ^
  CLI11.lib spdlog.lib fmt.lib OpenCL.lib avformat.lib avcodec.lib swresample.lib swscale.lib avutil.lib ^
  tinyexr.lib miniz.lib cudart_static.lib cuda.lib secur32.lib ncrypt.lib crypt32.lib shell32.lib ole32.lib ^
  user32.lib advapi32.lib
exit /b %ERRORLEVEL%
