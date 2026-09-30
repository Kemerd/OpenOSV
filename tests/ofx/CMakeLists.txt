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
#  shows".  So are two of the module's own host-independent sources
#  (OfxHost.cpp, OfxRender.cpp), for test_ofx_render.cpp: the host
#  classification and the CPU pixel loops in every format, tested directly.
#
#  Like the module, the tests need no Adobe SDK, so they run wherever the
#  bundle builds: Windows and macOS, CI included.  The [cuda] tests are
#  Windows-only (and SKIP without a device); the [sample] tests SKIP without
#  the sample clip.
#
#  THREE HOSTS, THREE REGISTRATIONS.  The module classifies its host once per
#  load, and a test process loads it once, so the mock plays one host per
#  process (OSV_MOCK_OFX_PROFILE, MockOfxHost.h):
#
#    <test name>           the generic mock (Resolve's behaviour): every test
#                          but the hidden [vegas] ones;
#    "vegas: <test name>"  VEGAS Pro: the [vegas] tests, with
#                          OSV_MOCK_OFX_PROFILE=vegas;
#    "resolve: <name>"     DaVinci Resolve by its own name: the descriptor
#                          snapshot, which must equal the generic one.
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
set(_osv_common_src "${CMAKE_SOURCE_DIR}/plugins/common")

# The CUDA tests exist where the module has a CUDA path: Windows with CUDA.
set(_osv_ofx_test_cuda OFF)
if(WIN32 AND OSV_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
  set(_osv_ofx_test_cuda ON)
endif()

add_executable(osv_ofx_tests
  TestMain.cpp
  MockOfxHost.cpp
  MockOfxHost.h
  OfxTestSupport.cpp
  OfxTestSupport.h
  test_ofx_module.cpp
  test_ofx_reframe.cpp
  test_ofx_source.cpp
  test_ofx_bundle.cpp
  test_ofx_snapshot.cpp
  test_ofx_render.cpp
  test_ofx_vegas.cpp
  "${_osv_reframe_src}/ReframeCpu.cpp"
  "${_osv_reframe_src}/ReframeEasing.cpp"
  # The module's host classification and CPU pixel loops, tested directly.
  "${_osv_ofx_src}/OfxHost.cpp"
  "${_osv_ofx_src}/OfxRender.cpp"
)
# ReframeCpu.cpp logs through PluginLog: the SDK-free implementation of
# each platform, as the module itself compiles it.
if(WIN32)
  target_sources(osv_ofx_tests PRIVATE "${_osv_common_src}/PluginLog.cpp")
else()
  target_sources(osv_ofx_tests PRIVATE "${_osv_common_src}/PluginLogPosix.cpp")
endif()
if(_osv_ofx_test_cuda)
  target_sources(osv_ofx_tests PRIVATE test_ofx_cuda.cpp)
endif()

target_include_directories(osv_ofx_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${_osv_ofx_src}"
  "${_osv_ofx_src}/openfx/include"
  "${_osv_reframe_src}"
  "${_osv_common_src}"
  "${CMAKE_SOURCE_DIR}/plugins/sourcesettings"
  # TestLogIsolation.h (Windows): keep the module's log out of the profile.
  "${CMAKE_SOURCE_DIR}/tests/premiere/mockhost"
)

target_link_libraries(osv_ofx_tests PRIVATE
  osv_warnings
  osv_render            # ReframeCpu.cpp and the library chain
  Catch2::Catch2
)
if(_osv_ofx_test_cuda)
  # The [cuda] tests play the host's CUDA context with the driver API.
  target_link_libraries(osv_ofx_tests PRIVATE CUDA::cuda_driver)
  target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_TEST_HAVE_CUDA=1)
endif()
if(WIN32 AND OSV_ENABLE_CUDA)
  # nvcuda.dll comes with the NVIDIA display driver, not the toolkit: a
  # machine (or CI runner) without one must still START the tests, which then
  # SKIP the [cuda] ones (OfxTestSupport.h, cudaDriverLoadable()).
  target_link_options(osv_ofx_tests PRIVATE "/DELAYLOAD:nvcuda.dll")
  target_link_libraries(osv_ofx_tests PRIVATE delayimp)
endif()
if(NOT WIN32)
  target_link_libraries(osv_ofx_tests PRIVATE ${CMAKE_DL_LIBS})  # dlopen
endif()

# /MD like the module: the two share one CRT, which is what lets the test's
# _putenv_s reach the module's getenv.
set_target_properties(osv_ofx_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# The module is loaded from where the build assembled it, never searched
# for: the .ofx inside the bundle (Contents/Win64 or Contents/MacOS).  A wide
# literal on Windows, where LoadLibraryExW takes one.
if(WIN32)
  target_compile_definitions(osv_ofx_tests PRIVATE "OSV_OFX_MODULE_PATH=L\"$<TARGET_FILE:OpenOSV>\"")
else()
  target_compile_definitions(osv_ofx_tests PRIVATE "OSV_OFX_MODULE_PATH=\"$<TARGET_FILE:OpenOSV>\"")
endif()
# The descriptor snapshot's golden file (test_ofx_snapshot.cpp).
target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden")
if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
  target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_SAMPLE_CLIP="${OSV_SAMPLE_FILE}")
endif()

add_dependencies(osv_ofx_tests OpenOSV)

catch_discover_tests(osv_ofx_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)

# -----------------------------------------------------------------------------
#  The bundle layout VEGAS Pro's plug-in scan needs (Windows).
#
#  VEGAS loads every DLL and .ofx under a bundle's Contents/Win64, so the
#  bundle keeps OpenOSV.ofx alone there and the dependency closure - with
#  osvtool.exe, which the VEGAS extension runs - in Contents/Libraries/Win64.
#    * ofx_bundle_layout   the STAGED tree: Win64 holds only OpenOSV.ofx,
#                          Libraries/Win64 holds osvtool.exe and every DLL the
#                          module and osvtool import;
#    * ofx_bundle_osvtool  osvtool.exe --help, started from Libraries/Win64
#                          with a PATH of System32 only, so a DLL
#                          missing from that folder cannot hide behind the
#                          developer's PATH.
#  (The delay-load half is the "[bundle]" test of osv_ofx_tests.)
# -----------------------------------------------------------------------------
if(WIN32 AND TARGET OpenOSVBundleTool AND OSV_DUMPBIN_EXE)
  get_target_property(_osv_ofx_test_bundle OpenOSV OSV_OFX_BUNDLE_DIR)
  add_test(NAME ofx_bundle_layout
    COMMAND "${CMAKE_COMMAND}"
            "-DBUNDLE_DIR=${_osv_ofx_test_bundle}"
            "-DDUMPBIN=${OSV_DUMPBIN_EXE}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/CheckBundleLayout.cmake")
  add_test(NAME ofx_bundle_osvtool
    COMMAND "${_osv_ofx_test_bundle}/Contents/Libraries/Win64/osvtool.exe" --help
    WORKING_DIRECTORY "${_osv_ofx_test_bundle}/Contents/Libraries/Win64")
  # A PATH of System32 alone (ENVIRONMENT is a list, so no ';' inside).
  set_tests_properties(ofx_bundle_osvtool PROPERTIES
    ENVIRONMENT "PATH=$ENV{SystemRoot}/System32"
    LABELS "ofx")
  set_tests_properties(ofx_bundle_layout PROPERTIES LABELS "ofx")
  # The bundle must be fully staged (module, Libraries, osvtool) first.
  add_dependencies(osv_ofx_tests OpenOSVBundleTool)
endif()

# VEGAS Pro: the hidden [vegas] tests, each in a process whose mock host says
# it is VEGAS (a different TEST_SPEC gives this registration files of its own).
# [WP-V-GPU] Under the VEGAS profile the effects frame CPU images on their own
# GPU by default; these are the CPU paths' tests, so OPENOSV_OFX_GPU=0 keeps
# them there on an NVIDIA machine too (the GPU path has its own [cuda] tests,
# which switch it on).  A second property rather than a second ENVIRONMENT
# entry: Catch's discovery flattens PROPERTIES into one list, which would
# split an ENVIRONMENT value at its ';'.
catch_discover_tests(osv_ofx_tests
  TEST_SPEC "[vegas]"
  TEST_PREFIX "vegas: "
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT "OSV_MOCK_OFX_PROFILE=vegas"
             ENVIRONMENT_MODIFICATION "OPENOSV_OFX_GPU=set:0"
  DISCOVERY_MODE PRE_TEST
)

# DaVinci Resolve by its own name: the descriptors must be the generic ones.
catch_discover_tests(osv_ofx_tests
  TEST_SPEC "[snapshot]"
  TEST_PREFIX "resolve: "
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4 ENVIRONMENT "OSV_MOCK_OFX_PROFILE=resolve"
  DISCOVERY_MODE PRE_TEST
)
