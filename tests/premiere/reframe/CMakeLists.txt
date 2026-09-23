# =============================================================================
#  osv_reframe_tests - the Open360Reframe.aex test executable.
#
#  The tests load the BUILT module with LoadLibraryW rather than linking its
#  objects: that is the only way to check what Premiere actually sees - the
#  exported symbol names, the PiPL resource, the flag words at run time and
#  the module's behaviour as one linked unit.
#
#  The geometry helpers (ReframeCpu.cpp) ARE compiled in as well, because the
#  parity tests need to call buildParams()/renderCpu() directly to produce
#  the reference the loaded module is compared against.  That is the same
#  source file the .aex contains, so the two cannot disagree.
# =============================================================================
#  The tests need the module they load, so with OSV_PREMIERE_REFRAME=OFF the
#  directory skips itself instead of failing the configure - otherwise the
#  option could never be turned off, which is exactly what someone working on
#  the importer alone wants to do (and what tests/premiere/importer does for
#  the mirror case).
if(NOT TARGET Open360Reframe)
  message(STATUS "tests/premiere/reframe skipped: OSV_PREMIERE_REFRAME is OFF")
  return()
endif()

set(_osv_reframe_src "${CMAKE_SOURCE_DIR}/plugins/reframe")

add_executable(osv_reframe_tests
  TestMain.cpp
  ReframeTestSupport.cpp
  ReframeTestSupport.h
  test_effect.cpp
  test_render.cpp
  test_gpu.cpp
  test_pipl.cpp
  test_ui.cpp
  test_direct.cpp
  # Compiled here too so the tests can call the geometry and the CPU
  # renderer directly; the .aex contains the identical object.
  "${_osv_reframe_src}/ReframeCpu.cpp"
  # The direct fisheye -> view path, for the same reason: test_direct.cpp
  # drives the builder and the CPU twin the .aex contains, against the
  # equirect path it must frame identically.
  "${_osv_reframe_src}/DirectRender.cpp"
  # The overlay's interaction core, for the same reason: test_ui.cpp pins the
  # drag-to-degrees mapping AND ITS SIGNS as pure arithmetic, with no host and
  # no window, against the very source the .aex contains.  ReframeUiEvent.cpp
  # is deliberately NOT compiled here - it is the PF_Cmd_EVENT / DrawBot shim
  # and is exercised only through the loaded module, which is the only way to
  # prove what Premiere will actually see.
  "${_osv_reframe_src}/ReframeUi.cpp"
)

target_include_directories(osv_reframe_tests PRIVATE "${_osv_reframe_src}")


# The [sample] direct-path tests need the 6K clip.  OSV_SAMPLE_FILE in the
# environment wins at run time; this compile-time default is only used when it
# is unset (and only defined when the file exists at configure time, the same
# rule tests/premiere/importer applies).
if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
  target_compile_definitions(osv_reframe_tests PRIVATE OSV_REFRAME_SAMPLE_CLIP="${OSV_SAMPLE_FILE}")
endif()

target_link_libraries(osv_reframe_tests PRIVATE
  osv_warnings
  osv_premiere_mockhost   # the fake host (brings osv_ae_sdk / osv_premiere_sdk)
  osv_premiere_common     # PluginLog, and the library through osv_render
  CUDA::cuda_driver       # the GPU tests drive the same driver API the effect does
  Catch2::Catch2
  version                 # VerQueryValue, for the VERSIONINFO check
)

# /MD to match the module, the mock host and the other test executables.
set_target_properties(osv_reframe_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# The tests need to know where the module was staged.  A generator
# expression is used rather than the stage directory variable so a target
# rename or an OUTPUT_DIRECTORY override cannot leave the tests looking in
# the wrong place.  The path is baked in as a wide literal because
# LoadLibraryW takes one.
target_compile_definitions(osv_reframe_tests PRIVATE
  "OSV_REFRAME_AEX_PATH=L\"$<TARGET_FILE:Open360Reframe>\""
)

# Without this the tests would silently run against a stale .aex.
add_dependencies(osv_reframe_tests Open360Reframe)

# SKIP_RETURN_CODE 4 is Catch2's SKIP(); ctest then reports "Not Run" instead
# of a failure for the [cuda] tests on a machine without a device.
catch_discover_tests(osv_reframe_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
