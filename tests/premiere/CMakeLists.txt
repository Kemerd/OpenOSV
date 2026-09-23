# =============================================================================
#  tests/premiere - the mock-host harness and its tests (see docs/PREMIERE.md,
#  "Verification").
#
#  This directory is added from plugins/CMakeLists.txt (not from
#  tests/CMakeLists.txt) because every target here needs the Adobe SDK
#  INTERFACE targets that cmake/OsvPremiereSdk.cmake only creates when
#  OSV_BUILD_PREMIERE=ON.  A plain library build therefore never sees it and
#  keeps building without any SDK.
#
#    mockhost/   osv_premiere_mockhost  static, Catch2-free: fake piSuites +
#                                       SPBasicSuite serving our own suites
#    common/     osv_premiere_common_tests  Catch2 v3 tests of the mock host
#                                           and of plugins/common
#
#    importer/        osv_importer_tests         loads OpenOSVImporter.prm
#    reframe/         osv_reframe_tests          loads Open360Reframe.aex
#    sourcesettings/  osv_source_settings_tests  loads OpenOSVSourceSettings.aex
#
#  Each of the three plug-in directories skips itself when its module is not
#  being built, so any single OSV_PREMIERE_* option can be turned off without
#  failing the configure.
# =============================================================================
if(NOT TARGET osv_premiere_sdk)
  message(FATAL_ERROR "tests/premiere requires the Adobe SDK targets (OSV_BUILD_PREMIERE=ON)")
endif()

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

# The mock host itself: no Catch2, so plug-in code can link it too.
add_subdirectory(mockhost)

# -----------------------------------------------------------------------------
#  osv_premiere_common_tests - mock host + plugins/common
# -----------------------------------------------------------------------------
add_executable(osv_premiere_common_tests
  common/TestMain.cpp
  common/test_mockhost.cpp
  common/test_common.cpp
  common/test_pixelcopy_bands.cpp
  common/test_user_defaults.cpp   # [WP-DEFAULTS] the per-user Source Settings defaults file
)

target_link_libraries(osv_premiere_common_tests PRIVATE
  osv_warnings
  osv_premiere_mockhost
  osv_premiere_common
  Catch2::Catch2
)

# /MD to match the plug-in modules and the mock host.
set_target_properties(osv_premiere_common_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

target_compile_definitions(osv_premiere_common_tests PRIVATE
  OSV_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output"
)
file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-output")

# The DelayLoad test needs a DLL sitting next to the test executable; a tiny
# module with one exported function is built for exactly that purpose.
add_library(osv_delayload_probe MODULE common/DelayLoadProbe.cpp)
set_target_properties(osv_delayload_probe PROPERTIES
  PREFIX ""
  OUTPUT_NAME "osv_delayload_probe"
  SUFFIX ".dll"
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  LIBRARY_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)
add_dependencies(osv_premiere_common_tests osv_delayload_probe)

# SKIP_RETURN_CODE 4 is Catch2's SKIP(); ctest then reports "Not Run" instead
# of a failure for the [cuda] tests on a machine without a device.
catch_discover_tests(osv_premiere_common_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)

# -----------------------------------------------------------------------------
#  Plug-in test executables from the next milestone step (added when present).
# -----------------------------------------------------------------------------
foreach(_dir importer reframe sourcesettings bench)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dir}/CMakeLists.txt")
    add_subdirectory(${_dir})
  endif()
endforeach()
