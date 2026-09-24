# =============================================================================
#  osv_ofx_tests - tests of the BUILT OpenOSV.ofx, through a mock OpenFX host.
#
#  Like the Premiere plug-ins' tests, the executable does not link the
#  module's objects: it LoadLibraryW's the bundle this build assembled and
#  drives it through the OpenFX C API (MockOfxHost.h), so what is tested is
#  what DaVinci Resolve loads - its exports, its descriptors, its parameters
#  and its pixels.
#
#  The Premiere effect's camera sources (ReframeCpu.cpp, ReframeEasing.cpp)
#  ARE compiled in: they produce the reference every OpenFX render is held
#  to, which is exactly the claim under test - "the same frame Premiere
#  shows".
# =============================================================================
if(NOT TARGET OpenOSV)
  message(STATUS "tests/ofx skipped: OSV_BUILD_OFX is OFF")
  return()
endif()

# Imported targets are scoped to the directory that finds them.
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

set(_osv_ofx_src "${CMAKE_SOURCE_DIR}/plugins/ofx")
set(_osv_reframe_src "${CMAKE_SOURCE_DIR}/plugins/reframe")

add_executable(osv_ofx_tests
  TestMain.cpp
  MockOfxHost.cpp
  MockOfxHost.h
  OfxTestSupport.cpp
  OfxTestSupport.h
  test_ofx_module.cpp
  test_ofx_reframe.cpp
  test_ofx_cuda.cpp
  test_ofx_source.cpp
  "${_osv_reframe_src}/ReframeCpu.cpp"
  "${_osv_reframe_src}/ReframeEasing.cpp"
)

target_include_directories(osv_ofx_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${_osv_ofx_src}"
  "${_osv_ofx_src}/openfx/include"
  "${_osv_reframe_src}"
  "${CMAKE_SOURCE_DIR}/plugins/sourcesettings"
  # TestLogIsolation.h: keep the module's log out of the user's profile.
  "${CMAKE_SOURCE_DIR}/tests/premiere/mockhost"
)

target_link_libraries(osv_ofx_tests PRIVATE
  osv_warnings
  osv_premiere_common   # PluginLog (ReframeCpu.cpp logs), the library chain
  CUDA::cuda_driver     # the [cuda] tests play the host's CUDA context
  Catch2::Catch2
)

# /MD like the module: the two share one CRT, which is what lets the test's
# _putenv_s reach the module's getenv.
set_target_properties(osv_ofx_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# The module is loaded from where the build assembled it, never searched for.
target_compile_definitions(osv_ofx_tests PRIVATE
  "OSV_OFX_MODULE_PATH=L\"$<TARGET_FILE:OpenOSV>\""
)
if(OSV_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
  target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_TEST_HAVE_CUDA=1)
endif()
if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
  target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_SAMPLE_CLIP="${OSV_SAMPLE_FILE}")
endif()

add_dependencies(osv_ofx_tests OpenOSV)

catch_discover_tests(osv_ofx_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
