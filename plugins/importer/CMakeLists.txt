# =============================================================================
#  OpenOSVImporter.prm - the standard file importer for .OSV / .LRF clips.
#
#  Built by osv_add_premiere_plugin(), which gives it the .prm suffix, no
#  "lib" prefix, /MD, /SUBSYSTEM:WINDOWS, the stage directory as its output
#  and a post-build step that copies every runtime DLL it imports next to it.
#
#  The module exports xImportEntry for Premiere and the three OsvEngine_*
#  entry points of the direct-GPU engine for the reframe effect
#  (plugins/common/OsvEngineAbi.h, Engine.cpp); everything else is internal.
#  There is no .def file (the Adobe samples use __declspec(dllexport) and so
#  do we), and no PiPL: standard importers need only the IMPT resource in
#  OpenOSVImporter.rc.
# =============================================================================

# The platform halves of the module:
#   * Windows: the direct-GPU engine and its OsvEngine_* exports (Engine.cpp,
#     CUDA driver API) and the resources (.rc: IMPT, the dialog template,
#     VERSIONINFO);
#   * macOS: no CUDA, so no direct path - EngineNoDirect.cpp answers the
#     importer's engine hooks truthfully for that build - and the IMPT
#     resource comes from OpenOSVImporter.r through Rez (osv_add_pipl below).
if(APPLE)
  set(_osv_importer_platform_sources EngineNoDirect.cpp)
else()
  set(_osv_importer_platform_sources Engine.cpp OpenOSVImporter.rc resource.h)
endif()

osv_add_premiere_plugin(OpenOSVImporter
  KIND prm
  EXPORTS xImportEntry
  SOURCES
    ImporterEntry.cpp
    ImporterInstance.cpp
    ImporterInstance.h
    ImporterVideo.cpp
    ImporterAudio.cpp
    ImporterAudio.h
    ImporterAudioSelectors.cpp
    ImporterPlugin.h
    PrefsMapping.cpp
    ProtectorGuard.cpp
    ProtectorGuard.h
    Engine.h
    FlareStage.cpp
    FlareStage.h
    SteadyStage.cpp
    SteadyStage.h
    ImporterGpuFrame.cpp
    ImporterGpuFrame.h
    SourceSettingsDialog.cpp
    ${_osv_importer_platform_sources}
)

target_link_libraries(OpenOSVImporter PRIVATE
  osv_premiere_common   # host suites, log, delay-load hook, pixel copy, host context, prefs
  osv_render            # pulls core, container, meta, color, video, geom
  osv_ffmpeg            # the importer decodes its own AAC with swresample
)

if(APPLE)
  # The IMPT resource Premiere recognises an importer by (Adobe's
  # SDK_File_ImportPiPL.r), compiled by Rez into Contents/Resources.
  osv_add_pipl(OpenOSVImporter
    R_FILE  "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSVImporter.r"
    OUT_VAR OSV_IMPORTER_RSRC)
  message(STATUS "  Importer target : OpenOSVImporter.bundle -> ${OSV_PLUGIN_STAGE_DIR}")
  # Nothing below applies to a Mac bundle: no Win32 dialog libraries, no
  # CUDA, no delay-load table (the bundle carries its own renamed FFmpeg).
  return()
endif()

# The Source Settings dialog needs the common-controls and the shell/user
# libraries; the rest of the module links only what osv_render drags in.
target_link_libraries(OpenOSVImporter PRIVATE comctl32 user32 gdi32)

# The direct-GPU engine (Engine.cpp) drives NVDEC and uploads its tables
# through the CUDA driver API in Premiere's own context.  nvcuda.dll is
# delay-loaded below, so a machine without an NVIDIA driver still loads the
# importer and simply never serves the direct path.
target_link_libraries(OpenOSVImporter PRIVATE CUDA::cuda_driver)

# The importer's own GPU frame path packs 16u / 8u frames on the device before
# the readback (ImporterPackKernel.h).  The kernel lives in its own small
# static library so that nvcc never sees the plug-in's MSVC-only compile
# options, and it takes the project's CUDA flags (-fmad=false: the codes must
# match PixelCopy's host conversion bit for bit).  The runtime it launches
# through is the statically linked cudart the CUDA renderer already brings
# into this module - no new DLL dependency.
if(TARGET osv_render_cuda)
  add_library(osv_importer_pack STATIC ImporterPackKernel.cu ImporterPackKernel.h)
  target_link_libraries(osv_importer_pack PRIVATE osv_cuda_flags CUDA::cudart_static)
  set_target_properties(osv_importer_pack PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CUDA_SEPARABLE_COMPILATION OFF
    POSITION_INDEPENDENT_CODE ON
    FOLDER "plugins"
  )
  target_link_libraries(OpenOSVImporter PRIVATE osv_importer_pack)
  target_compile_definitions(OpenOSVImporter PRIVATE OSV_IMPORTER_HAVE_PACK_KERNEL=1)
endif()

# -----------------------------------------------------------------------------
#  Delay loading
#
#  Premiere Pro's own application directory ships avcodec/avformat/avutil and
#  a CUDA runtime, and that directory is searched before ours.  Every DLL the
#  module imports from vcpkg is therefore delay-loaded and resolved from the
#  plug-in's own folder by the hook in plugins/common/DelayLoad.cpp (installed
#  from DllMain).  A missing OpenCL.dll then only fails the OpenCL render
#  path instead of refusing to load the plug-in at all.
#
#  The list is the exact set `dumpbin /dependents OpenOSVImporter.prm` reports
#  for the vcpkg DLLs; it is asserted at build time by the check below so a
#  new dependency cannot silently start binding to Adobe's copy.
# -----------------------------------------------------------------------------
# Verified with `dumpbin /DEPENDENTS OpenOSVImporter.prm`: these seven are
# exactly what the linked module imports from vcpkg.  (swscale, miniz and zlib
# are linked in but nothing the importer calls reaches them, so they never
# appear as imports; z.dll is still staged next to the module because
# avformat-63.dll pulls it in at load time.)
set(OSV_IMPORTER_DELAYLOAD_DLLS
  avcodec-63.dll
  avformat-63.dll
  avutil-61.dll
  swresample-7.dll
  OpenCL.dll
  fmt.dll
  spdlog.dll
  # The CUDA driver: resolved by the system (the hook finds no copy in the
  # plug-in folder and falls back), delay-loaded so a driver-less machine
  # still loads the importer.
  nvcuda.dll
)
osv_add_delayload(OpenOSVImporter DLL_NAMES ${OSV_IMPORTER_DELAYLOAD_DLLS})

# The audit that nothing outside this list (beyond the OS and the CRT) is
# imported directly runs automatically: osv_add_delayload() records the names
# on the target and osv_add_premiere_plugin()'s post-build step checks the
# linked module against them with dumpbin (cmake/OsvCheckDelayLoad.cmake).

# The module is staged where install_plugins.ps1 picks it up.
message(STATUS "  Importer target : OpenOSVImporter.prm -> ${OSV_PLUGIN_STAGE_DIR}")
