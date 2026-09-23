# =============================================================================
#  osv_importer_tests - tests of the BUILT OpenOSVImporter.prm.
#
#  The executable does not link the importer's objects; it LoadLibraryW's the
#  module that was just staged and calls xImportEntry through it.  That way the
#  tests prove the thing that ships: its exports, its resources, its delay-load
#  table and its behaviour under the mock host.
#
#  The one exception is plugins/importer/PrefsMapping.cpp, which is compiled
#  into the test as well.  It is a dependency-free translation unit (no Win32,
#  no suites, no ImporterInstance), and compiling it here is how the pure
#  PrefsBlob <-> dialog-control mapping gets tested at all: the shipping path
#  runs it from inside a modal DialogBoxParamW that no test can drive.
# =============================================================================
if(NOT TARGET OpenOSVImporter)
  message(STATUS "tests/premiere/importer skipped: OSV_PREMIERE_IMPORTER is OFF")
  return()
endif()

add_executable(osv_importer_tests
  TestMain.cpp
  ImporterHarness.cpp
  ImporterHarness.h
  test_importer.cpp
  test_importer_bitdepth.cpp
  test_engine.cpp
  test_engine_settings.cpp  # [WP-SETTINGS] file identity, instance precedence, per-frame settings
  test_flare_importer.cpp   # [WP-FLARE] sun ghost removal end to end
  test_look_paths.cpp       # [WP-LOOK] the DJI Studio look on the direct path's Rec.709 working space
  test_prefs_mapping.cpp
  test_reopen.cpp
  "${CMAKE_SOURCE_DIR}/plugins/importer/PrefsMapping.cpp"
)

target_include_directories(osv_importer_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_SOURCE_DIR}/plugins/importer"
)

target_link_libraries(osv_importer_tests PRIVATE
  osv_warnings
  osv_premiere_mockhost     # the fake host: piSuites + every suite the importer acquires
  osv_premiere_common       # PrefsBlob, HostSuites types used by ImporterPlugin.h
  Catch2::Catch2
  CUDA::cuda_driver         # test_engine.cpp: a private context, like Premiere's
)

# /MD, like the module it loads: the .prm and the test share one CRT, which is
# what lets the test's _putenv_s reach the plug-in's getenv_s.
set_target_properties(osv_importer_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# The module path is baked in rather than searched for at run time: the test
# must load THE module this build produced, never one left in the stage
# directory by an earlier configuration.
target_compile_definitions(osv_importer_tests PRIVATE
  OSV_IMPORTER_MODULE_PATH="$<TARGET_FILE:OpenOSVImporter>"
  OSV_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output"
)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-output")

# The [sample] tests need the 6K clip and (for one of them) the .LRF proxy
# beside it; both are cache variables the top-level CMakeLists resolves.  When
# a path is empty or the file is missing the corresponding tests SKIP, so a
# clone without the footage still runs everything else.
if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
  target_compile_definitions(osv_importer_tests PRIVATE OSV_SAMPLE_CLIP_PATH="${OSV_SAMPLE_FILE}")
endif()
if(OSV_SAMPLE_LRF_FILE AND EXISTS "${OSV_SAMPLE_LRF_FILE}")
  target_compile_definitions(osv_importer_tests PRIVATE OSV_SAMPLE_PROXY_PATH="${OSV_SAMPLE_LRF_FILE}")
endif()

# The test loads the .prm, so the module (and the DLLs staged beside it) must
# exist before the test runs.
add_dependencies(osv_importer_tests OpenOSVImporter)

# SKIP_RETURN_CODE 4 is Catch2's SKIP(); ctest reports "Not Run" instead of a
# failure for the [sample] tests on a machine without the clip.
catch_discover_tests(osv_importer_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
