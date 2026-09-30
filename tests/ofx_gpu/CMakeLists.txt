# =============================================================================
#  osv_ofx_gpu_tests - [WP-V-GPU] the OpenFX module's own GPU path, driven
#  directly (plugins/ofx/OfxGpuPipeline.h).
#
#  tests/ofx proves the BUILT OpenOSV.ofx through a mock host, which is what a
#  host loads - but it can only hand the effects the images the effects
#  accept.  The own-GPU path serves hosts that hand CPU images in four
#  formats and two levels (VEGAS Pro), so this executable compiles the
#  pipeline under the three hooks itself and drives it with every depth,
#  channel order, level, render window and pitch, against the CPU paths' own
#  pieces (ReframeCpu.cpp's camera, osv_kernel.h's per-pixel function,
#  OfxHostImage.h's levels and quantisation).
#
#  The kernels are the module's: the same OfxReframeKernel.cu, built into a
#  fatbin by the same recipe (cmake/OsvCudaFatbin.cmake) and loaded through
#  the same driver-API cache (OfxCuda.cpp).  Windows with CUDA only, like the
#  module's CUDA path; the [cuda] tests SKIP without an NVIDIA device.
#
#  Added from plugins/ofx/CMakeLists.txt (the module this tests), like
#  plugins/CMakeLists.txt adds tests/premiere.
# =============================================================================
if(NOT WIN32 OR NOT OSV_ENABLE_CUDA OR NOT CMAKE_CUDA_COMPILER)
  return()
endif()

find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)
include("${CMAKE_SOURCE_DIR}/cmake/OsvCudaFatbin.cmake")

set(_osv_gpu_ofx "${CMAKE_SOURCE_DIR}/plugins/ofx")
set(_osv_gpu_common "${CMAKE_SOURCE_DIR}/plugins/common")
set(_osv_gpu_reframe "${CMAKE_SOURCE_DIR}/plugins/reframe")

# The module's kernel file, embedded under the symbol OfxCuda.cpp loads.
osv_add_cuda_fatbin(OfxGpuTestKernel
  SOURCE   "${_osv_gpu_ofx}/OfxReframeKernel.cu"
  SYMBOL   kOsvOfxReframeFatbin
  OUT_CPP  _osv_gpu_fatbin_cpp
  INCLUDES "${CMAKE_SOURCE_DIR}/include" "${_osv_gpu_ofx}"
  DEPENDS  "${_osv_gpu_ofx}/OfxKernelAbi.h"
           "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"
           "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h")

add_executable(osv_ofx_gpu_tests
  TestMain.cpp
  test_ofx_gpu_pipeline.cpp
  # ---- the pipeline and its kernel cache, exactly as the module builds them ----
  "${_osv_gpu_ofx}/OfxGpuPipeline.cpp"
  "${_osv_gpu_ofx}/OfxCuda.cpp"
  ${_osv_gpu_fatbin_cpp}
  # ---- the reference: the Premiere effect's camera, and the plug-in log ----
  "${_osv_gpu_reframe}/ReframeCpu.cpp"
  "${_osv_gpu_common}/PluginLog.cpp"
)
target_include_directories(osv_ofx_gpu_tests PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${_osv_gpu_ofx}"
  "${_osv_gpu_ofx}/openfx/include"
  "${_osv_gpu_common}"
  "${_osv_gpu_reframe}"
  # TestLogIsolation.h: keep the plug-in log out of the user's profile.
  "${CMAKE_SOURCE_DIR}/tests/premiere/mockhost"
)
# The pipeline's CUDA half is compiled, as in the module.
target_compile_definitions(osv_ofx_gpu_tests PRIVATE OSV_OFX_HAVE_CUDA=1)
target_link_libraries(osv_ofx_gpu_tests PRIVATE
  osv_warnings
  osv_render            # ThreadPool, VirtualCamera (the camera), osv_kernel.h
  Catch2::Catch2
  CUDA::cuda_driver
  delayimp
)
# nvcuda.dll comes with the display driver: a machine without one must still
# START the tests (ctest lists them by running the executable) and SKIP.
target_link_options(osv_ofx_gpu_tests PRIVATE "/DELAYLOAD:nvcuda.dll")
set_target_properties(osv_ofx_gpu_tests PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  RUNTIME_OUTPUT_DIRECTORY "$<1:${CMAKE_BINARY_DIR}/bin>"
  FOLDER "tests"
)

catch_discover_tests(osv_ofx_gpu_tests
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
  PROPERTIES SKIP_RETURN_CODE 4
  DISCOVERY_MODE PRE_TEST
)
