# =============================================================================
#  OsvFFmpeg.cmake
#
#  Locates the vcpkg-provided FFmpeg libraries and exposes them as the
#  INTERFACE target `osv_ffmpeg`.
#
#  Licensing guard: OpenOSV is Apache-2.0 and links FFmpeg dynamically under
#  the LGPL.  A build that enables the `gpl` (or `nonfree`) vcpkg feature would
#  make the resulting binaries GPL, which is fine for private experiments but
#  must never happen by accident in a release, so we fail configuration if the
#  manifest features include them.
# =============================================================================
if(TARGET osv_ffmpeg)
  return()
endif()

foreach(_bad_feature IN ITEMS gpl nonfree all-gpl all-nonfree)
  if("${_bad_feature}" IN_LIST VCPKG_MANIFEST_FEATURES)
    message(FATAL_ERROR
      "vcpkg feature '${_bad_feature}' is enabled. OpenOSV must link an LGPL FFmpeg build. "
      "Remove the feature from VCPKG_MANIFEST_FEATURES.")
  endif()
endforeach()

# vcpkg ships a FindFFMPEG.cmake wrapper that fills FFMPEG_INCLUDE_DIRS,
# FFMPEG_LIBRARIES and FFMPEG_LIBRARY_DIRS for the enabled components.
find_package(FFMPEG REQUIRED)

add_library(osv_ffmpeg INTERFACE)
target_include_directories(osv_ffmpeg INTERFACE ${FFMPEG_INCLUDE_DIRS})
target_link_directories(osv_ffmpeg INTERFACE ${FFMPEG_LIBRARY_DIRS})
target_link_libraries(osv_ffmpeg INTERFACE ${FFMPEG_LIBRARIES})

# FFmpeg's public headers are C; make sure C++ consumers see the correct
# declarations of the stdint macros they rely on.
target_compile_definitions(osv_ffmpeg INTERFACE __STDC_CONSTANT_MACROS __STDC_LIMIT_MACROS)

message(STATUS "FFmpeg include dirs: ${FFMPEG_INCLUDE_DIRS}")
