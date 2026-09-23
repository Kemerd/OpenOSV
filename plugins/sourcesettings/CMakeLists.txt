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
# DllMain (the delay-load hook) and the .rc are Windows only; a macOS bundle
# gets its PiPL from Rez (osv_add_pipl below) and needs no hook.
if(APPLE)
  set(_osv_ss_platform_sources)
else()
  set(_osv_ss_platform_sources DllMain.cpp OpenOSVSourceSettings.rc)
endif()

osv_add_premiere_plugin(OpenOSVSourceSettings
  KIND aex
  EXPORTS EffectMain
  SOURCES
    SourceSettingsMain.cpp
    # The pure control <-> PrefsBlob arithmetic.  Its own translation unit so
    # the tests compile the very same source the .aex contains and can walk
    # every value of every prefs enum through it without a host - the same
    # split plugins/importer/PrefsMapping.cpp uses for the modal dialog.
    SourceSettingsMapping.cpp
    SourceSettingsMapping.h
    SourceSettingsParams.h
    ${_osv_ss_platform_sources}
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
#  Only two entries, and the short list is the point: this module renders
#  nothing, so it pulls in none of the library's FFmpeg / OpenCL / CUDA
#  machinery the way plugins/reframe does.  What it does use is PluginLog,
#  which goes through spdlog and fmt - and Adobe ships its own copies of
#  common libraries in the application directory, which Windows searches
#  before a plug-in's own folder.  Loading Premiere's fmt.dll against our
#  spdlog build is exactly the silent ABI mismatch that crashes inside the
#  logger on the first message, so both are delay-loaded and resolved from
#  this module's folder by plugins/common/DelayLoad.cpp.
#
#  Listing them explicitly rather than inferring them from the configuration
#  is deliberate: a Debug / RelWithDebInfo link surfaces these imports where
#  a Release link may inline the calls away, and an entry for a DLL the
#  module happens not to import is harmless (the linker reports it as unused
#  and the POST_BUILD audit prints it), while a MISSING entry is a load
#  failure on a user's machine.
# -----------------------------------------------------------------------------
osv_add_delayload(OpenOSVSourceSettings DLL_NAMES fmt.dll spdlog.dll)

message(STATUS "OpenOSVSourceSettings")
message(STATUS "  Match name    : OpenOSV.SourceSettings")
message(STATUS "  Delay-loaded  : fmt.dll, spdlog.dll")
