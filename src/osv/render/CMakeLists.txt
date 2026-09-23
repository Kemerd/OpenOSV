# =============================================================================
#  osv_render_cpu    - reference renderer + parameter builder (always built)
#  osv_render_cuda   - CUDA backend  (OSV_ENABLE_CUDA)
#  osv_render_opencl - OpenCL backend (OSV_ENABLE_OPENCL)
#
#  All three execute include/osv/render/osv_kernel.h.  The CUDA and OpenCL
#  targets depend on osv_render_cpu so RendererFactory can fall back.
# =============================================================================
file(GLOB OSV_RENDER_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/render/*.h")
file(GLOB OSV_RENDER_CPU_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_render_cpu STATIC ${OSV_RENDER_CPU_SOURCES} ${OSV_RENDER_HEADERS})
add_library(osv::render_cpu ALIAS osv_render_cpu)
target_link_libraries(osv_render_cpu PUBLIC osv_core osv_geom osv_color osv_video)
set_target_properties(osv_render_cpu PROPERTIES FOLDER "osv")

# Neural flow backend (FlowBackendOnnx.cpp, part of the GLOB above).  ONNX
# Runtime is optional: without it OSV_HAVE_ONNXRUNTIME stays undefined, that
# file compiles to nothing, and the Neural backend reports itself absent.
# Only headers are needed here - the DLL is loaded at runtime by full path
# (see cmake/OsvOnnxRuntime.cmake for why no import library is linked).
include(OsvOnnxRuntime)
if(TARGET osv_onnxruntime)
  target_link_libraries(osv_render_cpu PRIVATE osv_onnxruntime)
  add_dependencies(osv_render_cpu osv_flow_model)
endif()

# ---------------------------------------------------------------------------
#  CUDA backend
# ---------------------------------------------------------------------------
if(OSV_ENABLE_CUDA AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/cuda/CudaKernel.cu")
  file(GLOB OSV_RENDER_CUDA_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_CURRENT_SOURCE_DIR}/cuda/*.cu" "${CMAKE_CURRENT_SOURCE_DIR}/cuda/*.cpp")
  add_library(osv_render_cuda STATIC ${OSV_RENDER_CUDA_SOURCES})
  add_library(osv::render_cuda ALIAS osv_render_cuda)
  target_link_libraries(osv_render_cuda PUBLIC osv_render_cpu osv_cuda_flags CUDA::cudart_static CUDA::cuda_driver)
  target_compile_definitions(osv_render_cuda PUBLIC OSV_HAVE_CUDA=1)
  set_target_properties(osv_render_cuda PROPERTIES
    FOLDER "osv"
    CUDA_SEPARABLE_COMPILATION OFF
    CUDA_RESOLVE_DEVICE_SYMBOLS ON)
endif()

# ---------------------------------------------------------------------------
#  OpenCL backend: the kernel source is embedded at build time.  Three
#  strings are handed to clCreateProgramWithSource in order:
#     preamble.cl  -> ColorMath.h -> osv_kernel.h -> kernel.cl
# ---------------------------------------------------------------------------
if(OSV_ENABLE_OPENCL AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/opencl/OpenClRenderer.cpp")
  set(_ocl_gen "${CMAKE_BINARY_DIR}/generated/osv/render/opencl")
  file(MAKE_DIRECTORY "${_ocl_gen}")
  osv_embed_text_file("${_ocl_gen}/embedded_preamble.cpp"  "${CMAKE_CURRENT_SOURCE_DIR}/opencl/preamble.cl"            osv_cl_preamble)
  osv_embed_text_file("${_ocl_gen}/embedded_colormath.cpp" "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h"       osv_cl_colormath)
  osv_embed_text_file("${_ocl_gen}/embedded_kernel_h.cpp"  "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"     osv_cl_kernel_h)
  osv_embed_text_file("${_ocl_gen}/embedded_kernel_cl.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/opencl/kernel.cl"              osv_cl_kernel_cl)
  file(GLOB OSV_RENDER_OPENCL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/opencl/*.cpp")
  add_library(osv_render_opencl STATIC
    ${OSV_RENDER_OPENCL_SOURCES}
    "${_ocl_gen}/embedded_preamble.cpp"
    "${_ocl_gen}/embedded_colormath.cpp"
    "${_ocl_gen}/embedded_kernel_h.cpp"
    "${_ocl_gen}/embedded_kernel_cl.cpp")
  add_library(osv::render_opencl ALIAS osv_render_opencl)
  target_link_libraries(osv_render_opencl PUBLIC osv_render_cpu osv_opencl)
  target_compile_definitions(osv_render_opencl PUBLIC OSV_HAVE_OPENCL=1)
  set_target_properties(osv_render_opencl PROPERTIES FOLDER "osv")
endif()

# ---------------------------------------------------------------------------
#  Metal backend (macOS): the same four-part assembly as OpenCL,
#     preamble.metal -> ColorMath.h -> osv_kernel.h -> kernel.metal
#  concatenated into one source (cmake/OsvMetal.cmake), embedded, and - when
#  the Metal compiler is installed - also compiled ahead of time into a
#  metallib that is embedded next to it.
# ---------------------------------------------------------------------------
if(OSV_ENABLE_METAL AND APPLE AND TARGET osv_metal AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/metal/MetalRenderer.mm")
  set(_mtl_gen "${CMAKE_BINARY_DIR}/generated/osv/render/metal")
  osv_metal_library("${_mtl_gen}" osv_kernels
    SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/metal/preamble.metal"
            "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h"
            "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"
            "${CMAKE_CURRENT_SOURCE_DIR}/metal/kernel.metal"
    OUT_SOURCE _mtl_source
    OUT_METALLIB _mtl_metallib)
  osv_embed_text_file("${_mtl_gen}/embedded_metal_source.cpp" "${_mtl_source}" osv_metal_source)
  set(_mtl_embedded "${_mtl_gen}/embedded_metal_source.cpp")
  if(_mtl_metallib)
    osv_embed_text_file("${_mtl_gen}/embedded_metal_lib.cpp" "${_mtl_metallib}" osv_metal_lib)
    list(APPEND _mtl_embedded "${_mtl_gen}/embedded_metal_lib.cpp")
  endif()
  add_library(osv_render_metal STATIC "${CMAKE_CURRENT_SOURCE_DIR}/metal/MetalRenderer.mm" ${_mtl_embedded})
  add_library(osv::render_metal ALIAS osv_render_metal)
  target_link_libraries(osv_render_metal PUBLIC osv_render_cpu osv_metal)
  target_compile_definitions(osv_render_metal PUBLIC OSV_HAVE_METAL=1)
  if(_mtl_metallib)
    target_compile_definitions(osv_render_metal PRIVATE OSV_METAL_HAVE_METALLIB=1)
  endif()
  # ARC: every Metal object in the backend is reference counted by the
  # compiler, so no path through the renderer can leak or over-release one.
  set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/metal/MetalRenderer.mm" PROPERTIES
    COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(osv_render_metal PROPERTIES FOLDER "osv")
endif()

# ---------------------------------------------------------------------------
#  Umbrella target: RendererFactory (makeRenderer) links every backend that
#  was built.  Tools and tests link osv_render only.
# ---------------------------------------------------------------------------
add_library(osv_render STATIC "${CMAKE_CURRENT_SOURCE_DIR}/factory/RendererFactory.cpp")
add_library(osv::render ALIAS osv_render)
target_link_libraries(osv_render PUBLIC osv_render_cpu)
if(TARGET osv_render_cuda)
  target_link_libraries(osv_render PUBLIC osv_render_cuda)
endif()
if(TARGET osv_render_opencl)
  target_link_libraries(osv_render PUBLIC osv_render_opencl)
endif()
if(TARGET osv_render_metal)
  target_link_libraries(osv_render PUBLIC osv_render_metal)
endif()
set_target_properties(osv_render PROPERTIES FOLDER "osv")
