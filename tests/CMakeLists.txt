# =============================================================================
#  OpenOSV test-suite (Catch2 v3)
#
#  Tags:
#    [sample]     needs the sample .OSV clip (SKIPs when absent)
#    [cuda]       needs a CUDA device
#    [opencl]     needs an OpenCL device
#    [hwaccel]    needs a hardware video decoder
#    [ffmpeg-exe] needs ffmpeg.exe on PATH (or OSV_FFMPEG_EXE)
#  Catch2's SKIP() exits with code 4, which is registered below so ctest shows
#  skipped tests as "Not Run" rather than failures.
# =============================================================================
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)

file(GLOB OSV_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/unit/*.cpp")

add_executable(osv_tests TestMain.cpp ${OSV_TEST_SOURCES})
target_include_directories(osv_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

# Link every module that exists; tests for modules not yet written simply do
# not exist in the glob above.
set(_osv_test_libs osv_core)
foreach(_m container meta color video geom render io)
  if(TARGET osv_${_m})
    list(APPEND _osv_test_libs osv_${_m})
  endif()
endforeach()
target_link_libraries(osv_tests PRIVATE ${_osv_test_libs} Catch2::Catch2 nlohmann_json::nlohmann_json)

target_compile_definitions(osv_tests PRIVATE
  OSV_SAMPLE_FILE="${OSV_SAMPLE_FILE}"
  OSV_SAMPLE_LRF_FILE="${OSV_SAMPLE_LRF_FILE}"
  OSV_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/golden"
  OSV_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  # The committed .cube LUTs (build output of scripts/gen_luts.ps1); the cube
  # test regenerates one and compares it against the committed copy.
  OSV_LUTS_DIR="${CMAKE_SOURCE_DIR}/luts"
  OSV_TEST_OUTPUT_DIR="${CMAKE_BINARY_DIR}/test-output"
)
if(TARGET osvtool)
  target_compile_definitions(osv_tests PRIVATE OSV_TOOL_PATH="$<TARGET_FILE:osvtool>")
  add_dependencies(osv_tests osvtool)
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-output")

catch_discover_tests(osv_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)

# -----------------------------------------------------------------------------
#  osv_gpu_analysis_bench - what one parallax measurement costs on the CPU and
#  on the GPU (docs/DIRECT_GPU.md, WP-B; see bench/GpuAnalysisBench.cpp).
#
#  A measurement tool, NOT a test: its numbers depend on the machine and it
#  asserts nothing, so it is built with the suite (always against the modules
#  just built) but never registered with ctest.
# -----------------------------------------------------------------------------
if(TARGET osv_render_cuda)
  add_executable(osv_gpu_analysis_bench bench/GpuAnalysisBench.cpp)
  target_link_libraries(osv_gpu_analysis_bench PRIVATE ${_osv_test_libs})
  target_compile_definitions(osv_gpu_analysis_bench PRIVATE OSV_SAMPLE_FILE="${OSV_SAMPLE_FILE}")
  set_target_properties(osv_gpu_analysis_bench PROPERTIES FOLDER "tests")
  # FFmpeg and friends beside the executable, as osvtool does.
  add_custom_command(TARGET osv_gpu_analysis_bench POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:osv_gpu_analysis_bench>
            $<TARGET_FILE_DIR:osv_gpu_analysis_bench>
    COMMAND_EXPAND_LISTS)
endif()
