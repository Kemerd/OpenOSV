# =============================================================================
#  OpenOSVImporter.prm - the standard file importer for .OSV / .LRF clips.
#
#  Built by osv_add_premiere_plugin(), which gives it the .prm suffix, no
#  "lib" prefix, /MD, /SUBSYSTEM:WINDOWS, the stage directory as its output
#  and a post-build step that copies every runtime DLL it imports next to it.
#
#  The module exports exactly one symbol, xImportEntry; everything else is
#  internal.  There is no .def file (the Adobe samples use __declspec(dllexport)
#  and so do we), and no PiPL: standard importers need only the IMPT resource
#  in OpenOSVImporter.rc.
# =============================================================================

osv_add_premiere_plugin(OpenOSVImporter
  KIND prm
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
    SourceSettingsDialog.cpp
    OpenOSVImporter.rc
    resource.h
)

target_link_libraries(OpenOSVImporter PRIVATE
  osv_premiere_common   # host suites, log, delay-load hook, pixel copy, host context, prefs
  osv_render            # pulls core, container, meta, color, video, geom
  osv_ffmpeg            # the importer decodes its own AAC with swresample
)

# The Source Settings dialog needs the common-controls and the shell/user
# libraries; the rest of the module links only what osv_render drags in.
target_link_libraries(OpenOSVImporter PRIVATE comctl32 user32 gdi32)

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
)
osv_add_delayload(OpenOSVImporter DLL_NAMES ${OSV_IMPORTER_DELAYLOAD_DLLS})

# The audit that nothing outside this list (beyond the OS and the CRT) is
# imported directly runs automatically: osv_add_delayload() records the names
# on the target and osv_add_premiere_plugin()'s post-build step checks the
# linked module against them with dumpbin (cmake/OsvCheckDelayLoad.cmake).

# The module is staged where install_plugins.ps1 picks it up.
message(STATUS "  Importer target : OpenOSVImporter.prm -> ${OSV_PLUGIN_STAGE_DIR}")
