# =============================================================================
#  osv_premiere_common - the host-facing layer shared by both plug-ins.
#
#  Static, /MD, links the milestone 1 library (osv_render pulls the whole
#  chain: core, container, meta, color, video, geom) plus the Premiere SDK
#  headers.  Nothing here includes an AE header; the AE side of the effect
#  stays in plugins/reframe.
#
#  Every file is listed explicitly (no GLOB) because the plug-in targets
#  depend on the exact set of translation units, in particular DelayLoad.cpp
#  which defines the linker-level delay-load hook.
# =============================================================================
add_library(osv_premiere_common STATIC
  HostContext.cpp
  HostContext.h
  HostSuites.cpp
  HostSuites.h
  HostUtf16.h
  NumberParse.h
  PixelCopy.cpp
  PixelCopy.h
  PluginExport.h
  PluginLog.h
  PrefsBlob.h
  # The per-user Source Settings defaults file (%APPDATA%\OpenOSV\defaults.json).
  # SDK-free on purpose: osvtool compiles the same file for --use-user-defaults.
  UserDefaults.cpp
  UserDefaults.h
)
add_library(osv::premiere_common ALIAS osv_premiere_common)

# The platform halves.  Windows: the delay-load hook (a macOS bundle carries
# uniquely named copies of its libraries instead, cmake/OsvMacBundle.cmake)
# and the log with its OutputDebugString copy; macOS: the same log contract
# on POSIX files and the unified log.
if(WIN32)
  target_sources(osv_premiere_common PRIVATE DelayLoad.cpp DelayLoad.h PluginLog.cpp)
else()
  target_sources(osv_premiere_common PRIVATE PluginLogPosix.cpp)
endif()

target_include_directories(osv_premiere_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(osv_premiere_common
  PUBLIC
    osv_warnings
    osv_core
    osv_render
    osv_premiere_sdk
  PRIVATE
    # UserDefaults.cpp reads and writes the defaults file as JSON.
    nlohmann_json::nlohmann_json
)

# The plug-ins are /MD modules; the static library must use the same CRT.
set_target_properties(osv_premiere_common PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  POSITION_INDEPENDENT_CODE ON
  FOLDER "plugins"
)
