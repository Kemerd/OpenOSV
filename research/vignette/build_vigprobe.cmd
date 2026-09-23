@echo off
rem Disposable: compile vigprobe.cpp against a release build's static
rem libraries.  B defaults to this checkout's build tree; pass another build
rem directory as the first argument to link against it instead.
setlocal
set "ROOT=%~dp0..\.."
set "B=%ROOT%\build\windows-msvc-premiere-release"
if not "%~1"=="" set "B=%~1"
set "V=%B%\vcpkg_installed\x64-windows"
if not exist "%V%\include" set "V=%ROOT%\vcpkg_installed\x64-windows"
set "CU=C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v12.9"
set "OUT=%~dp0bin"
if not exist "%OUT%" mkdir "%OUT%"
cl /nologo /EHsc /O2 /MD -std:c++20 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /bigobj ^
  -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_CRT_SECURE_NO_WARNINGS -DCLI11_COMPILE ^
  -DOSV_HAVE_COLOR=1 -DOSV_HAVE_CONTAINER=1 -DOSV_HAVE_CUDA=1 -DOSV_HAVE_GEOM=1 -DOSV_HAVE_IO=1 ^
  -DOSV_HAVE_META=1 -DOSV_HAVE_OPENCL=1 -DOSV_HAVE_RENDER=1 -DOSV_HAVE_VIDEO=1 -DOSV_VIDEO_HAVE_CUDA=1 ^
  -DCL_HPP_MINIMUM_OPENCL_VERSION=120 -DCL_HPP_TARGET_OPENCL_VERSION=120 -DCL_TARGET_OPENCL_VERSION=120 ^
  -D__STDC_CONSTANT_MACROS -D__STDC_LIMIT_MACROS ^
  -I"%ROOT%\include" -I"%B%\generated" -I"%V%\include" -I"%CU%\include" ^
  "%~dp0vigprobe.cpp" ^
  "%B%\tools\osvtool\CMakeFiles\osvtool.dir\Pipeline.cpp.obj" ^
  /Fo"%OUT%\\" /Fe"%OUT%\vigprobe.exe" ^
  /link /LIBPATH:"%V%\lib" /LIBPATH:"%CU%\lib\x64" ^
  "%B%\lib\osv_io.lib" "%B%\lib\osv_render.lib" "%B%\lib\osv_render_cuda.lib" "%B%\lib\osv_render_opencl.lib" ^
  "%B%\lib\osv_render_cpu.lib" "%B%\lib\osv_video.lib" "%B%\lib\osv_geom.lib" "%B%\lib\osv_color.lib" ^
  "%B%\lib\osv_meta.lib" "%B%\lib\osv_container.lib" "%B%\lib\osv_core.lib" ^
  CLI11.lib spdlog.lib fmt.lib OpenCL.lib avformat.lib avcodec.lib swresample.lib swscale.lib avutil.lib ^
  tinyexr.lib miniz.lib cudart_static.lib cuda.lib secur32.lib ncrypt.lib crypt32.lib shell32.lib ole32.lib ^
  user32.lib advapi32.lib
exit /b %ERRORLEVEL%
