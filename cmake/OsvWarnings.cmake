# =============================================================================
#  OsvWarnings.cmake
#
#  Defines the INTERFACE target `osv_warnings` that every OpenOSV target links
#  against.  It carries the compiler flags that make the CPU reference renderer
#  reproducible (strict floating point) and the warning level we build with.
#
#  Floating point policy:
#    * MSVC  : /fp:precise  - no contraction of a*b+c into an FMA, no reassoc
#    * CUDA  : -fmad=false  - same, applied in OsvCuda.cmake
#    * OpenCL: no -cl-fast-relaxed-math, applied at clBuildProgram time
#  Keeping all three on the same rules is what lets the CPU/GPU parity tests
#  demand PSNR >= 60 dB after 16-bit quantisation.
# =============================================================================
if(TARGET osv_warnings)
  return()
endif()

add_library(osv_warnings INTERFACE)

if(MSVC)
  target_compile_options(osv_warnings INTERFACE
    $<$<COMPILE_LANG_AND_ID:CXX,MSVC>:
      /W4                 # high warning level
      /permissive-        # standards conformance
      /utf-8              # source and execution charset UTF-8
      /Zc:__cplusplus     # report the real __cplusplus value
      /Zc:preprocessor    # conforming preprocessor
      /EHsc               # C++ exceptions only (we never throw across APIs, but STL needs it)
      /fp:precise         # reproducible floating point for the reference renderer
      /bigobj             # large template-heavy translation units (Catch2)
      /wd4201             # nameless struct/union (used by small math types)
      /wd4324             # structure padded due to alignment specifier
    >
    $<$<COMPILE_LANG_AND_ID:C,MSVC>:/W4 /utf-8 /fp:precise>
  )
  target_compile_definitions(osv_warnings INTERFACE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    _SILENCE_ALL_CXX17_DEPRECATION_WARNINGS
    UNICODE
    _UNICODE
  )
  if(OSV_WARNINGS_AS_ERRORS)
    target_compile_options(osv_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:/WX>)
  endif()
else()
  # Non-MSVC hosts are not a supported target for milestone 1, but keep the
  # flags sane so the CPU-only library still compiles with clang/gcc.
  target_compile_options(osv_warnings INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -Wpedantic -ffp-contract=off>
  )
  if(OSV_WARNINGS_AS_ERRORS)
    target_compile_options(osv_warnings INTERFACE $<$<COMPILE_LANGUAGE:CXX>:-Werror>)
  endif()
endif()

# The generated Version.h lives in the build tree.
target_include_directories(osv_warnings INTERFACE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)
