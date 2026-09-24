# =============================================================================
#  OsvOpenCL.cmake
#
#  Locates the OpenCL headers and ICD loader from vcpkg (port `opencl`) and
#  exposes them as the INTERFACE target `osv_opencl`.
#
#  OpenCL is the vendor-neutral GPU backend: Premiere Pro hands AMD / Intel
#  plug-ins an OpenCL context on Windows, and NVIDIA parts still expose a full
#  OpenCL 3.0 runtime, so the same renderer runs everywhere.  We target OpenCL
#  1.2 API semantics (the lowest common denominator of shipping drivers) and
#  compile the kernel from source at run time with strict floating-point flags.
# =============================================================================
if(TARGET osv_opencl)
  return()
endif()

find_package(OpenCL REQUIRED)

add_library(osv_opencl INTERFACE)
target_link_libraries(osv_opencl INTERFACE OpenCL::OpenCL)
target_compile_definitions(osv_opencl INTERFACE
  CL_TARGET_OPENCL_VERSION=120
  CL_HPP_TARGET_OPENCL_VERSION=120
  CL_HPP_MINIMUM_OPENCL_VERSION=120
)

# macOS: CMake finds Apple's OpenCL.framework (OpenCL 1.2, implemented on top
# of Metal).  Apple marks every entry point deprecated; the backend is a
# vendor-neutral fallback there, so the warnings carry no information.
if(APPLE)
  target_compile_definitions(osv_opencl INTERFACE CL_SILENCE_DEPRECATION)
endif()

message(STATUS "OpenCL: ${OpenCL_INCLUDE_DIRS} (${OpenCL_VERSION_STRING})")
