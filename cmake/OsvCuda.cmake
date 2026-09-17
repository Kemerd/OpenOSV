# =============================================================================
#  OsvCuda.cmake
#
#  Enables the CUDA language and pins the flags that keep the CUDA renderer
#  numerically aligned with the CPU reference renderer:
#
#    -fmad=false           no fused multiply-add contraction (matches /fp:precise)
#    no --use_fast_math    keep IEEE division/sqrt and precise transcendentals
#    no -ftz               denormals behave like on the host
#
#  Architecture policy: SASS for Turing (75), Ampere (86), Ada (89) and
#  Blackwell (120) plus PTX for 120 so future parts JIT.  sm_120 needs a
#  CUDA >= 12.8 toolkit; the RTX 5090 development machine ships 12.9.
#
#  The machine used for development has a stale CUDA 10.1 nvcc first on PATH;
#  CMakePresets.json therefore forces CMAKE_CUDA_COMPILER from CUDA_PATH.
# =============================================================================
if(NOT CMAKE_CUDA_ARCHITECTURES)
  set(CMAKE_CUDA_ARCHITECTURES "75-real;86-real;89-real;120-real;120-virtual")
endif()

enable_language(CUDA)
set(CMAKE_CUDA_STANDARD 20)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

find_package(CUDAToolkit 12.8 REQUIRED)

if(CUDAToolkit_VERSION VERSION_LESS 12.8)
  message(FATAL_ERROR
    "CUDA ${CUDAToolkit_VERSION} found but sm_120 (RTX 50 series) needs CUDA 12.8 or newer. "
    "Point CMAKE_CUDA_COMPILER at a newer nvcc.")
endif()

add_library(osv_cuda_flags INTERFACE)
target_compile_options(osv_cuda_flags INTERFACE
  $<$<COMPILE_LANGUAGE:CUDA>:
    -fmad=false
    --expt-relaxed-constexpr
    -Xcompiler=/utf-8
    -Xcompiler=/W3
    -Xcompiler=/EHsc
    -Xcompiler=/fp:precise
    -Xcudafe=--diag_suppress=20012   # host/device annotations on defaulted functions
  >
)

# nvcc refuses host compilers newer than the ones it was qualified with.
# CUDA 12.9 supports MSVC up to 19.44 (VS 17.14); allow newer ones explicitly
# so that a Visual Studio update does not break the build outright.
if(MSVC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER "19.44.99999")
  target_compile_options(osv_cuda_flags INTERFACE $<$<COMPILE_LANGUAGE:CUDA>:-allow-unsupported-compiler>)
  message(WARNING "MSVC ${CMAKE_CXX_COMPILER_VERSION} is newer than the CUDA-qualified toolset; "
                  "building with -allow-unsupported-compiler.")
endif()

message(STATUS "CUDA toolkit ${CUDAToolkit_VERSION} (${CMAKE_CUDA_COMPILER}), archs: ${CMAKE_CUDA_ARCHITECTURES}")
