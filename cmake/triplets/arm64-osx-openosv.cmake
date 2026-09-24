# =============================================================================
#  arm64-osx-openosv  (vcpkg overlay triplet, macOS on Apple Silicon)
#
#  vcpkg's stock arm64-osx triplet links every port statically.  That is what
#  OpenOSV wants for the small C++ libraries (fmt, spdlog, Catch2, tinyexr):
#  each binary carries its own copy, nothing extra has to travel next to
#  osvtool or inside a plug-in bundle, and two plug-ins loaded into one host
#  process never share a logger registry.
#
#  FFmpeg is the exception.  OpenOSV is Apache-2.0 and uses FFmpeg under the
#  LGPL, which the project honours by linking it DYNAMICALLY on every platform
#  (cmake/OsvFFmpeg.cmake explains the licensing guard).  So the ffmpeg port
#  alone is switched to dynamic linkage here; vcpkg rewrites its install names
#  to @rpath, which is what lets osvtool and the plug-in bundles find their own
#  copies next to themselves.
#
#  Deployment target 13.3: libc++ marks the floating-point std::to_chars that
#  std::format relies on as available from macOS 13.3, and the library formats
#  doubles in its diagnostics.  Every port is built for the same floor so the
#  linker never warns about objects "built for a newer macOS version".
# =============================================================================
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)

set(VCPKG_CMAKE_SYSTEM_NAME Darwin)
set(VCPKG_OSX_ARCHITECTURES arm64)
set(VCPKG_OSX_DEPLOYMENT_TARGET 13.3)

# LGPL: FFmpeg is always a shared library, never folded into our binaries.
if(PORT STREQUAL "ffmpeg")
  set(VCPKG_LIBRARY_LINKAGE dynamic)
endif()
