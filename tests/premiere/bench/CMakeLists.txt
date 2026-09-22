# =============================================================================
#  osv_importer_bench - where a frame's time goes (see ImporterBench.cpp).
#
#  A measurement tool, NOT a test: it is built with the Premiere tests so it
#  always matches the modules they exercise, but it is deliberately not
#  registered with ctest.  Its numbers depend on the machine, it runs for tens
#  of seconds on real 6K footage, and it asserts nothing.
#
#  It reuses three things rather than duplicating them:
#    * the importer test harness (mock host + the real OpenOSVImporter.prm);
#    * osvtool's Pipeline, the object graph the importer's own setup mirrors,
#      so the stage replay decodes and stitches through the same calls;
#    * the reframe effect's ReframeCpu.cpp, for the effect-side timings.
#  None of those sources is modified; they are only compiled in here.
# =============================================================================
if(NOT TARGET OpenOSVImporter OR NOT TARGET Open360Reframe)
  message(STATUS "tests/premiere/bench skipped: needs both the importer and the reframe effect")
  return()
endif()
if(NOT TARGET osv_render_cuda)
  message(STATUS "tests/premiere/bench skipped: needs the CUDA renderer (OSV_ENABLE_CUDA)")
  return()
endif()

set(_osv_src "${CMAKE_SOURCE_DIR}")

add_executable(osv_importer_bench
  ImporterBench.cpp
  "${_osv_src}/tests/premiere/importer/ImporterHarness.cpp"
  "${_osv_src}/tools/osvtool/Pipeline.cpp"
  "${_osv_src}/plugins/reframe/ReframeCpu.cpp"
)

target_include_directories(osv_importer_bench PRIVATE
  "${_osv_src}/tests/premiere/importer"   # ImporterHarness.h
  "${_osv_src}/plugins/importer"          # ImporterPlugin.h, pulled in by the harness
  "${_osv_src}/plugins/reframe"           # ReframeCpu.h / ReframeParams.h
  "${_osv_src}/tools/osvtool"             # Pipeline.h
  "${_osv_src}/src/osv/render/cuda"       # CudaLaunch.h: the effect kernel's launcher
)

# Pipeline.cpp is compiled exactly as osvtool compiles it, so it needs the
# same libraries and the same OSV_HAVE_* switches.
set(_bench_libs osv_warnings osv_premiere_mockhost osv_premiere_common osv_core CLI11::CLI11)
foreach(_m container meta color video geom render io)
  if(TARGET osv_${_m})
    list(APPEND _bench_libs osv_${_m})
    string(TOUPPER "${_m}" _M)
    target_compile_definitions(osv_importer_bench PRIVATE OSV_HAVE_${_M}=1)
  endif()
endforeach()
list(APPEND _bench_libs osv_render_cuda)
target_link_libraries(osv_importer_bench PRIVATE ${_bench_libs})

target_compile_definitions(osv_importer_bench PRIVATE
  OSV_IMPORTER_MODULE_PATH="$<TARGET_FILE:OpenOSVImporter>"
  OSV_BENCH_DEFAULT_CLIP="${_osv_src}/example_footage_dlogm.OSV"
)

set_target_properties(osv_importer_bench PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

# Always measure the modules that were just built, never a stale one.
add_dependencies(osv_importer_bench OpenOSVImporter Open360Reframe)
