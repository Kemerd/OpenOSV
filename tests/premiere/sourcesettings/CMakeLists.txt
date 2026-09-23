# =============================================================================
#  osv_source_settings_tests - the OpenOSVSourceSettings.aex test executable.
#
#  The tests load the BUILT module with LoadLibraryW rather than linking its
#  objects: that is the only way to check what Premiere actually sees - the
#  exported symbol name, the PiPL resource (whose match name is the ENTIRE
#  binding to the importer), the flag words at run time and the module's
#  behaviour as one linked unit.
#
#  SourceSettingsMapping.cpp IS compiled in as well, because the pure
#  control <-> PrefsBlob arithmetic is where a mistake is both easiest to make
#  and hardest to see (AE popup values are 1-based, the blob's enums are
#  0-based), and driving every combination of every enum through the loaded
#  module would be thousands of host calls for something that is arithmetic.
#  It is the same source file the .aex contains, so the two cannot disagree.
# =============================================================================
#  With OSV_PREMIERE_SOURCE_SETTINGS=OFF the directory skips itself instead of
#  failing the configure - otherwise the option could never be turned off,
#  which is exactly what someone working on the importer alone wants to do.
if(NOT TARGET OpenOSVSourceSettings)
  message(STATUS "tests/premiere/sourcesettings skipped: OSV_PREMIERE_SOURCE_SETTINGS is OFF")
  return()
endif()

set(_osv_ss_src "${CMAKE_SOURCE_DIR}/plugins/sourcesettings")

add_executable(osv_source_settings_tests
  TestMain.cpp
  SourceSettingsTestSupport.cpp
  SourceSettingsTestSupport.h
  test_params.cpp
  test_prefs.cpp
  test_pipl.cpp
  test_defaults.cpp   # [WP-DEFAULTS] the Defaults group and new-clip seeding
  # Compiled here too so the tests can exercise the mapping directly; the
  # .aex contains the identical object.
  "${_osv_ss_src}/SourceSettingsMapping.cpp"
)

target_include_directories(osv_source_settings_tests PRIVATE
  "${_osv_ss_src}"
  "${CMAKE_SOURCE_DIR}/plugins/common"   # SourceSettingsIdentity.h, PrefsBlob.h
)

target_link_libraries(osv_source_settings_tests PRIVATE
  osv_warnings
  osv_premiere_mockhost   # the fake host (brings osv_ae_sdk / osv_premiere_sdk)
  osv_premiere_common     # PluginLog, PrefsBlob
  Catch2::Catch2
  version                 # VerQueryValue, for the VERSIONINFO check
)

# /MD to match the module, the mock host and the other test executables.
set_target_properties(osv_source_settings_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# The tests need to know where the module was staged.  A generator expression
# rather than the stage directory variable, so a target rename or an
# OUTPUT_DIRECTORY override cannot leave the tests looking in the wrong place.
# The path is baked in as a wide literal because LoadLibraryW takes one.
target_compile_definitions(osv_source_settings_tests PRIVATE
  "OSV_SOURCE_SETTINGS_AEX_PATH=L\"$<TARGET_FILE:OpenOSVSourceSettings>\""
)

# Without this the tests would silently run against a stale .aex.
add_dependencies(osv_source_settings_tests OpenOSVSourceSettings)

# SKIP_RETURN_CODE 4 is Catch2's SKIP(); none of these cases skip today, but
# the property is set for consistency with the sibling test targets so a
# future hardware-gated case behaves the same way.
catch_discover_tests(osv_source_settings_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
