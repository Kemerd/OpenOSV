# =============================================================================
#  OpenOSV Premiere Pro plug-ins (milestone 2, see docs/PREMIERE.md)
#
#    common/     osv_premiere_common   static: host suites, log, delay-load,
#                                      pixel copy, host context, prefs blob
#    importer/   OpenOSVImporter.prm   (OSV_PREMIERE_IMPORTER)
#    reframe/    Open360Reframe.aex    (OSV_PREMIERE_REFRAME)
#
#  Only reached when OSV_BUILD_PREMIERE=ON; cmake/OsvPremiereSdk.cmake has
#  already validated the SDK paths and created osv_premiere_sdk / osv_ae_sdk.
# =============================================================================
option(OSV_PREMIERE_IMPORTER "Build the OpenOSV importer (.prm)"        ON)
option(OSV_PREMIERE_REFRAME  "Build the Open 360 Reframe effect (.aex)" ON)

if(NOT TARGET osv_premiere_sdk)
  message(FATAL_ERROR "plugins/ requires OSV_BUILD_PREMIERE=ON (cmake/OsvPremiereSdk.cmake defines osv_premiere_sdk)")
endif()

# The shared layer is always built: the mock-host tests exercise it even
# when neither plug-in is enabled.
add_subdirectory(common)

# The two plug-in directories are added when their option is ON.  They are
# created by the next milestone step; until then a missing directory is
# reported instead of failing the configure.
set(_osv_plugin_status "")
if(OSV_PREMIERE_IMPORTER)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/importer/CMakeLists.txt")
    add_subdirectory(importer)
    set(_osv_importer_status "ON")
  else()
    message(STATUS "plugins/importer not present yet - importer target skipped")
    set(_osv_importer_status "ON (directory missing, skipped)")
  endif()
else()
  set(_osv_importer_status "OFF")
endif()

if(OSV_PREMIERE_REFRAME)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/reframe/CMakeLists.txt")
    add_subdirectory(reframe)
    set(_osv_reframe_status "ON")
  else()
    message(STATUS "plugins/reframe not present yet - reframe target skipped")
    set(_osv_reframe_status "ON (directory missing, skipped)")
  endif()
else()
  set(_osv_reframe_status "OFF")
endif()

# Mock-host test harness for the plug-ins.  It lives under tests/premiere so
# the test tree mirrors the plug-in tree; it is added from here because it
# only makes sense together with the SDK targets.
if(OSV_BUILD_TESTS)
  add_subdirectory("${CMAKE_SOURCE_DIR}/tests/premiere" "${CMAKE_BINARY_DIR}/tests/premiere")
endif()

# -----------------------------------------------------------------------------
#  Summary
# -----------------------------------------------------------------------------
message(STATUS "Premiere plug-ins")
message(STATUS "  Stage directory : ${OSV_PLUGIN_STAGE_DIR}")
message(STATUS "  Premiere SDK    : IMPORTMOD_VERSION ${OSV_PREMIERE_SDK_IMPORTMOD_VERSION}")
message(STATUS "  AE SDK          : effect spec ${OSV_AE_SDK_SPEC_VERSION}")
message(STATUS "  Importer (.prm) : ${_osv_importer_status}")
message(STATUS "  Reframe (.aex)  : ${_osv_reframe_status}")
message(STATUS "  Mock-host tests : ${OSV_BUILD_TESTS}")
