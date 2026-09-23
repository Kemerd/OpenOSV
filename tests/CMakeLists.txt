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

# The GPU decoder tests drive the CUDA driver API directly (contexts,
# pointer attributes, device-to-host downloads of the cached frames).
if(OSV_ENABLE_CUDA AND TARGET CUDA::cuda_driver)
  target_link_libraries(osv_tests PRIVATE CUDA::cuda_driver)
endif()

# -----------------------------------------------------------------------------
#  osv_gpu_decode_bench: GpuClipDecoder timings (open, park cold / session /
#  hit, sequential with and without decode-ahead, VRAM).  Run by hand; it is
#  deliberately NOT registered with ctest because timings on a shared machine
#  are not pass/fail material.
# -----------------------------------------------------------------------------
if(OSV_ENABLE_CUDA AND TARGET CUDA::cuda_driver)
  add_executable(osv_gpu_decode_bench bench/GpuDecodeBench.cpp)
  target_link_libraries(osv_gpu_decode_bench PRIVATE osv_video osv_meta osv_container osv_core CUDA::cuda_driver)
  target_compile_definitions(osv_gpu_decode_bench PRIVATE OSV_SAMPLE_FILE="${OSV_SAMPLE_FILE}")
  set_target_properties(osv_gpu_decode_bench PROPERTIES FOLDER "osv/bench")
endif()

catch_discover_tests(osv_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
