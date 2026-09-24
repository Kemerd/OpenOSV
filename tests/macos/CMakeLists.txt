# =============================================================================
#  tests/macos - the SDK-free half of the macOS Premiere plug-ins, tested on
#  any Mac (a CI runner included) without the Adobe SDKs.
#
#  The plug-ins cannot be built without Adobe's headers, and those may not
#  be redistributed, so a CI machine without them would otherwise never
#  compile a line of the Mac-specific plug-in code.  Most of that code does
#  not touch the SDK at all, though, and is compiled and exercised here from
#  the very sources the bundles contain:
#
#    osv_plugin_portable_tests
#      * plugins/common: PluginLogPosix.cpp (the log), HostContext.cpp (the
#        Metal-first renderer choice), HostUtf16.h, NumberParse.h;
#      * plugins/importer/EngineNoDirect.cpp;
#      * plugins/reframe: ReframeCpu.cpp, ReframeEasing.cpp, ReframeUi.cpp and
#        the whole Metal GPU path (ReframeMetal.mm + ReframeKernel.metal,
#        through osv_reframe_metal), checked against the CPU path the effect
#        falls back to;
#    osv_bundle_probe.bundle
#      * a tiny bundle built by the SAME osv_add_mac_bundle() as the
#        plug-ins: its FFmpeg is embedded under OpenOSV_ names, it exports one
#        entry point, it is signed - and the tests load it and check all of
#        that, which is the part of the Mac packaging most likely to go
#        wrong silently.
# =============================================================================
if(NOT APPLE)
  return()
endif()

include(OsvMacBundle)
include("${CMAKE_SOURCE_DIR}/plugins/reframe/ReframeMetal.cmake")

set(_osv_plugins "${CMAKE_SOURCE_DIR}/plugins")

# -----------------------------------------------------------------------------
#  The probe bundle
# -----------------------------------------------------------------------------
osv_add_mac_bundle(osv_bundle_probe
  EXTENSION bundle
  PACKAGE_TYPE BNDL
  SIGNATURE "????"
  OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests/macos/probe"
  EXPORTS osvBundleProbeAvutilVersion osvBundleProbeAvcodecImage
  SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/BundleProbe.cpp")
target_link_libraries(osv_bundle_probe PRIVATE osv_warnings osv_ffmpeg)
target_include_directories(osv_bundle_probe PRIVATE "${_osv_plugins}/common")
set_target_properties(osv_bundle_probe PROPERTIES FOLDER "tests")

# -----------------------------------------------------------------------------
#  The tests
# -----------------------------------------------------------------------------
add_executable(osv_plugin_portable_tests
  "${CMAKE_SOURCE_DIR}/tests/TestMain.cpp"
  test_plugin_text.cpp
  test_plugin_log.cpp
  test_engine_no_direct.cpp
  test_host_context.cpp
  test_reframe_metal.mm
  test_bundle.cpp
  # The plug-in sources under test, exactly as the bundles compile them.
  "${_osv_plugins}/common/PluginLogPosix.cpp"
  "${_osv_plugins}/common/HostContext.cpp"
  "${_osv_plugins}/importer/EngineNoDirect.cpp"
  "${_osv_plugins}/reframe/ReframeCpu.cpp"
  "${_osv_plugins}/reframe/ReframeEasing.cpp"
  "${_osv_plugins}/reframe/ReframeUi.cpp"
)
target_include_directories(osv_plugin_portable_tests PRIVATE
  "${CMAKE_SOURCE_DIR}/tests"
  "${_osv_plugins}/common"
  "${_osv_plugins}/importer"
  "${_osv_plugins}/reframe")
target_link_libraries(osv_plugin_portable_tests PRIVATE
  osv_render osv_reframe_metal osv_metal Catch2::Catch2 "-framework Foundation" "-framework Metal")
set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/test_reframe_metal.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
target_compile_definitions(osv_plugin_portable_tests PRIVATE
  OSV_BUNDLE_PROBE_BINARY="$<TARGET_FILE:osv_bundle_probe>"
  OSV_BUNDLE_PROBE_DIR="$<TARGET_BUNDLE_DIR:osv_bundle_probe>")
add_dependencies(osv_plugin_portable_tests osv_bundle_probe)
set_target_properties(osv_plugin_portable_tests PROPERTIES FOLDER "tests")

catch_discover_tests(osv_plugin_portable_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
