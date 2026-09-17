# =============================================================================
#  OpenOSVSourceSettings.aex - the master clip Source Settings effect
#  (docs/PREMIERE.md, "Source Settings effect")
#
#  One module, one entry point (EffectMain) and one PiPL.  It is a SEPARATE
#  .aex from Open360Reframe deliberately, for two independent reasons:
#
#    1. the AE SDK lists "multiple PiPLs in a single plug-in" among the
#       features Premiere does not support, so two effects cannot share one
#       module even if we wanted them to;
#    2. the two effects are different KINDS of effect.  A source settings
#       effect is never sent PF_Cmd_RENDER and its parameters cannot be
#       keyframed (they travel to the importer as one flat prefs blob), while
#       the reframe effect is a normal timeline filter whose whole point is
#       keyframed Pan / Tilt / Roll / FOV.  See SourceSettingsParams.h.
#
#  Compared with plugins/reframe this build is simple: no CUDA kernel, no
#  fatbin, no GPU entry point.  Just the module, its PiPL and the delay-load
#  hook.
# =============================================================================

# -----------------------------------------------------------------------------
#  1. The module
# -----------------------------------------------------------------------------
osv_add_premiere_plugin(OpenOSVSourceSettings
  KIND aex
  SOURCES
    DllMain.cpp
    SourceSettingsMain.cpp
    # The pure control <-> PrefsBlob arithmetic.  Its own translation unit so
    # the tests compile the very same source the .aex contains and can walk
    # every value of every prefs enum through it without a host - the same
    # split plugins/importer/PrefsMapping.cpp uses for the modal dialog.
    SourceSettingsMapping.cpp
    SourceSettingsMapping.h
    SourceSettingsParams.h
    OpenOSVSourceSettings.rc
)

target_include_directories(OpenOSVSourceSettings PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(OpenOSVSourceSettings PRIVATE
  osv_premiere_common   # PluginLog, PrefsBlob, the delay-load hook
)

# -----------------------------------------------------------------------------
#  2. The PiPL
#
#  The .r includes SourceSettingsParams.h (and through it
#  SourceSettingsIdentity.h), so editing a flag or the match name regenerates
#  the resource - osv_add_pipl records the dependency through cl /showIncludes,
#  and the two DEPENDS entries below make the header edits explicit as well.
#  RC_FILE ties the .rc recompile to the generated .rcp.
# -----------------------------------------------------------------------------
osv_add_pipl(OpenOSVSourceSettings
  R_FILE  "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSVSourceSettings.r"
  RC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSVSourceSettings.rc"
  OUT_VAR OSV_SOURCE_SETTINGS_PIPL_RCP
  # The cl /EP pass is a standalone invocation and does not read the target's
  # include directories, so plugins/common has to be named here for the .r's
  # transitive #include "SourceSettingsIdentity.h" to resolve.
  INCLUDES "${CMAKE_SOURCE_DIR}/plugins/common"
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/SourceSettingsParams.h"
          "${CMAKE_SOURCE_DIR}/plugins/common/SourceSettingsIdentity.h")

# -----------------------------------------------------------------------------
#  3. Delay loading
#
#  The effect itself needs nothing beyond the Windows API, but it links
#  osv_premiere_common, which pulls osv_render and with it the library's
#  FFmpeg / OpenCL users, plus spdlog and fmt for PluginLog.  Delay-loading
#  every one of them means:
#    * a machine with no OpenCL ICD still loads the effect;
#    * the vcpkg copies beside the plug-in win over the older ones Premiere
#      ships in its own application directory (the DelayLoad hook resolves
#      from the module's folder first).
#
#  The names must match the DLL file names exactly.  The FFmpeg ones carry a
#  major version in the file name, so they are globbed from the installed
#  vcpkg tree rather than guessed - the same approach plugins/reframe uses.
# -----------------------------------------------------------------------------
set(_osv_ss_delayload "")
if(OSV_ENABLE_OPENCL)
  list(APPEND _osv_ss_delayload OpenCL.dll)
endif()
if(OSV_ENABLE_CUDA)
  # osv_render's CUDA renderer is reachable through the static library even
  # though this module never calls it, so the import has to be satisfied -
  # and delay-loading it means a driver-less machine still loads the effect.
  list(APPEND _osv_ss_delayload nvcuda.dll)
endif()
foreach(_ff IN ITEMS avcodec avformat avutil swresample swscale)
  file(GLOB _ff_dlls "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/${_ff}-*.dll")
  foreach(_ff_dll IN LISTS _ff_dlls)
    get_filename_component(_ff_name "${_ff_dll}" NAME)
    list(APPEND _osv_ss_delayload "${_ff_name}")
  endforeach()
endforeach()
# PluginLog goes through spdlog/fmt.  Listed explicitly rather than inferred
# from the configuration: a Debug/RelWithDebInfo link surfaces them where a
# Release link may not.
list(APPEND _osv_ss_delayload fmt.dll spdlog.dll)
list(REMOVE_DUPLICATES _osv_ss_delayload)
osv_add_delayload(OpenOSVSourceSettings DLL_NAMES ${_osv_ss_delayload})

message(STATUS "OpenOSVSourceSettings")
message(STATUS "  Match name    : OpenOSV.SourceSettings")
message(STATUS "  Delay-loaded  : ${_osv_ss_delayload}")
