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
#
#  Like the module, the tests need no Adobe SDK, so they run wherever the
#  bundle builds: Windows and macOS, CI included.  The [cuda] tests are
#  Windows-only (and SKIP without a device); the [sample] tests SKIP without
#  the sample clip.
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
  "${_osv_reframe_src}/ReframeCpu.cpp"
  "${_osv_reframe_src}/ReframeEasing.cpp"
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
if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
  target_compile_definitions(osv_ofx_tests PRIVATE OSV_OFX_SAMPLE_CLIP="${OSV_SAMPLE_FILE}")
endif()

add_dependencies(osv_ofx_tests OpenOSV)

catch_discover_tests(osv_ofx_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
