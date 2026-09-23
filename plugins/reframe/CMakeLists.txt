# =============================================================================
#  Open360Reframe.aex - the reframing effect (docs/PREMIERE.md, "Effect design")
#
#  One module with TWO entry points:
#    EffectMain        the After Effects API effect (parameters, UI, CPU render)
#    xGPUFilterEntry   the PrGPUFilter GPU renderer, bound by match name
#
#  The build has three unusual pieces, each explained at its site below:
#    1. the PiPL pipeline (osv_add_pipl) - Premiere's own Cnvtpipl.exe cannot
#       parse an AE-kind PiPL, so the AE SDK's PiPLtool.exe is used;
#    2. the CUDA fatbin - ReframeKernel.cu is NOT linked into the module; it
#       is compiled to a fatbin and embedded as bytes, so the .aex imports
#       nothing from the CUDA runtime and loads on a machine with no CUDA;
#    3. delay loading - anything the module imports beyond the Windows API is
#       resolved from the plug-in's own folder (plugins/common/DelayLoad.cpp).
#
#  macOS builds Open360Reframe.plugin instead, in the branch right below:
#  Premiere hands GPU effects Metal there, so the GPU path is the Metal
#  twin of the CUDA kernel (ReframeKernel.metal, ReframeMetal.mm), the PiPL
#  goes through Rez, and the direct path - CUDA only - is not built.
# =============================================================================

if(APPLE)
  include("${CMAKE_CURRENT_SOURCE_DIR}/ReframeMetal.cmake")

  osv_add_premiere_plugin(Open360Reframe
    KIND aex
    EXPORTS EffectMain xGPUFilterEntry
    SOURCES
      EffectMain.cpp
      GpuFilter.cpp
      ReframeCpu.cpp
      ReframeCpu.h
      ReframeParams.h
      ReframeEasing.cpp
      ReframeEasing.h
      ReframeUi.cpp
      ReframeUi.h
      ReframeUiEvent.cpp
      ReframeUiEvent.h
  )
  target_include_directories(Open360Reframe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
  target_link_libraries(Open360Reframe PRIVATE
    osv_premiere_common   # log, host context (brings osv_render)
    osv_reframe_metal     # the Metal GPU path and its embedded kernel library
  )

  osv_add_pipl(Open360Reframe
    R_FILE  "${CMAKE_CURRENT_SOURCE_DIR}/Open360Reframe.r"
    OUT_VAR OSV_REFRAME_PIPL_RSRC
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ReframeParams.h")

  message(STATUS "Open360Reframe")
  message(STATUS "  Match name    : OpenOSV.Open360Reframe")
  message(STATUS "  GPU path      : Metal (${OSV_PLUGIN_STAGE_DIR}/Open360Reframe.plugin)")
  return()
endif()

# -----------------------------------------------------------------------------
#  1. The CUDA kernel -> fatbin -> embedded byte array
#
#  nvcc is invoked directly (not through CMake's CUDA language support)
#  because a fatbin is not a linkable object and CMake has no notion of one.
#  The flags mirror cmake/OsvCuda.cmake so the GPU result stays inside the
#  parity budget the tests assert:
#     -fatbin        emit a fat binary, not an object
#     -fmad=false    no fused multiply-add contraction (matches /fp:precise)
#     -O3            the kernel is tiny and entirely arithmetic
#     no fast math   IEEE division / sqrt and precise transcendentals
#
#  Architectures: PTX for compute_50 (the SDK's documented minimum, so the
#  driver can JIT for anything newer that we did not build a cubin for) plus
#  real cubins for every architecture the installed toolkit accepts, probed
#  once at configure time.
# -----------------------------------------------------------------------------
if(NOT OSV_ENABLE_CUDA)
  message(FATAL_ERROR
    "plugins/reframe needs OSV_ENABLE_CUDA=ON: the effect's GPU path is CUDA only "
    "(Premiere dropped OpenCL rendering in 15.4 and DirectX is not released for third parties). "
    "Configure with -DOSV_PREMIERE_REFRAME=OFF to skip the effect instead.")
endif()
if(NOT CMAKE_CUDA_COMPILER)
  message(FATAL_ERROR "plugins/reframe needs a CUDA compiler (CMAKE_CUDA_COMPILER is empty)")
endif()

set(_osv_reframe_kernel_src "${CMAKE_CURRENT_SOURCE_DIR}/ReframeKernel.cu")
set(_osv_reframe_gen_dir    "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(_osv_reframe_fatbin     "${_osv_reframe_gen_dir}/ReframeKernel.fatbin")
set(_osv_reframe_fatbin_cpp "${_osv_reframe_gen_dir}/ReframeKernelFatbin.cpp")
file(MAKE_DIRECTORY "${_osv_reframe_gen_dir}")

# Which real architectures can this toolkit emit?  Probing beats hard-coding:
# a CUDA 13 toolkit drops sm_50..sm_70 and a CUDA 12.0 one has no sm_120, and
# either would fail the build over an architecture we do not actually need.
# The list is the union of everything Premiere-class hardware uses.
set(_osv_reframe_arch_candidates 60 61 70 75 80 86 89 90 120)
set(_osv_reframe_gencode_args "")
set(_osv_reframe_arch_report "")

# A one-line empty kernel is enough to tell whether nvcc accepts a -gencode
# flag; compiling the real kernel for every candidate would take minutes.
set(_probe_src "${_osv_reframe_gen_dir}/arch_probe.cu")
file(WRITE "${_probe_src}" "__global__ void osvArchProbe() {}\n")

foreach(_arch IN LISTS _osv_reframe_arch_candidates)
  execute_process(
    COMMAND "${CMAKE_CUDA_COMPILER}" -fatbin -Wno-deprecated-gpu-targets
            "-gencode" "arch=compute_${_arch},code=sm_${_arch}"
            -o "${_osv_reframe_gen_dir}/arch_probe_${_arch}.fatbin" "${_probe_src}"
    RESULT_VARIABLE _probe_rc
    OUTPUT_QUIET ERROR_QUIET)
  if(_probe_rc EQUAL 0)
    list(APPEND _osv_reframe_gencode_args "-gencode" "arch=compute_${_arch},code=sm_${_arch}")
    list(APPEND _osv_reframe_arch_report "sm_${_arch}")
  endif()
endforeach()
if(NOT _osv_reframe_gencode_args)
  message(FATAL_ERROR
    "nvcc at ${CMAKE_CUDA_COMPILER} accepted none of the candidate architectures "
    "(${_osv_reframe_arch_candidates}); the reframe kernel cannot be built.")
endif()

# PTX for the SDK's documented minimum compute capability, so a device we
# built no cubin for still runs (the driver JITs it on first load).
set(_osv_reframe_ptx_arch 50)
execute_process(
  COMMAND "${CMAKE_CUDA_COMPILER}" -fatbin -Wno-deprecated-gpu-targets
          "-gencode" "arch=compute_${_osv_reframe_ptx_arch},code=compute_${_osv_reframe_ptx_arch}"
          -o "${_osv_reframe_gen_dir}/ptx_probe.fatbin" "${_probe_src}"
  RESULT_VARIABLE _ptx_rc OUTPUT_QUIET ERROR_QUIET)
if(_ptx_rc EQUAL 0)
  list(INSERT _osv_reframe_gencode_args 0
       "-gencode" "arch=compute_${_osv_reframe_ptx_arch},code=compute_${_osv_reframe_ptx_arch}")
  list(INSERT _osv_reframe_arch_report 0 "compute_${_osv_reframe_ptx_arch} (PTX)")
endif()

add_custom_command(
  OUTPUT  "${_osv_reframe_fatbin}"
  COMMAND "${CMAKE_CUDA_COMPILER}"
          -fatbin -O3 -fmad=false -Wno-deprecated-gpu-targets
          ${_osv_reframe_gencode_args}
          "-I${CMAKE_SOURCE_DIR}/include"
          "-I${CMAKE_CURRENT_SOURCE_DIR}"
          -o "${_osv_reframe_fatbin}" "${_osv_reframe_kernel_src}"
  DEPENDS "${_osv_reframe_kernel_src}"
          "${CMAKE_CURRENT_SOURCE_DIR}/DirectKernelAbi.h"
          "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"
          "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h"
  COMMENT "nvcc ReframeKernel.cu -> fatbin (${_osv_reframe_arch_report})"
  VERBATIM)

# EmbedKernelScript.cmake reads its input as raw bytes (file(READ ... HEX)),
# so it embeds a fatbin just as happily as it embeds OpenCL source; the extra
# NUL it appends is harmless because the loader is driven by the size.
include("${CMAKE_SOURCE_DIR}/cmake/EmbedKernel.cmake")
osv_embed_text_file("${_osv_reframe_fatbin_cpp}" "${_osv_reframe_fatbin}" "kOsvReframeFatbin")

# -----------------------------------------------------------------------------
#  2. The module itself
# -----------------------------------------------------------------------------
osv_add_premiere_plugin(Open360Reframe
  KIND aex
  SOURCES
    DllMain.cpp
    EffectMain.cpp
    GpuFilter.cpp
    ReframeCpu.cpp
    ReframeCpu.h
    ReframeParams.h
    # [WP-EASING] The Keyframe Easing curves and keyframe walk: pure, shared
    # by the CPU and GPU readers, and compiled into the tests as well.
    ReframeEasing.cpp
    ReframeEasing.h
    # The direct fisheye -> view path (docs/DIRECT_GPU.md, WP-C): the
    # parameter builder and CPU twin, and the driver-API launcher of the
    # fused kernel that ReframeKernel.cu adds to the same fatbin.
    DirectRender.cpp
    DirectRender.h
    DirectLaunch.cpp
    DirectLaunch.h
    DirectKernelAbi.h
    # ...and the effect's side of the engine: source binding through the
    # segment graph, the working colour space, and the per-frame call into
    # the importer's OsvEngine_* exports (DirectPath.h).
    DirectPath.cpp
    DirectPath.h
    # [WP-SETTINGS] Which clips the direct path may render given their
    # Source Settings and the working colour space; pure, so the tests
    # compile the same source.
    DirectPathSettings.cpp
    DirectPathSettings.h
    # The interactive Program Monitor overlay.  Two translation units on
    # purpose: ReframeUi.cpp is the pure interaction maths and names no Adobe
    # type, so the tests compile the very same source the .aex contains and
    # pin the drag-to-degrees mapping without a host.  ReframeUiEvent.cpp is
    # the PF_Cmd_EVENT / DrawBot shim and is only ever in the module.
    ReframeUi.cpp
    ReframeUi.h
    ReframeUiEvent.cpp
    ReframeUiEvent.h
    Open360Reframe.rc
    "${_osv_reframe_fatbin_cpp}"
)

# The generated sources are produced by the custom commands above.
set_source_files_properties("${_osv_reframe_fatbin_cpp}" PROPERTIES GENERATED TRUE)

# The reframe tests compile the SAME embedded fatbin into their executable, so
# they can load the kernels the .aex carries without reaching into the module
# (kOsvReframeFatbin is not exported, and must not be).  Publishing the path
# on the target keeps the tests from hard-coding this directory's layout.
set_target_properties(Open360Reframe PROPERTIES OSV_REFRAME_FATBIN_CPP "${_osv_reframe_fatbin_cpp}")

target_include_directories(Open360Reframe PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(Open360Reframe PRIVATE
  osv_premiere_common   # log, delay-load hook, host context (brings osv_render)
  CUDA::cuda_driver     # nvcuda.dll only: no cudart in this module
)

# -----------------------------------------------------------------------------
#  3. The PiPL
#
#  The .r includes ReframeParams.h, so editing a flag or the match name
#  regenerates the resource (osv_add_pipl records the dependency through
#  cl /showIncludes); RC_FILE ties the .rc recompile to the generated .rcp.
# -----------------------------------------------------------------------------
osv_add_pipl(Open360Reframe
  R_FILE  "${CMAKE_CURRENT_SOURCE_DIR}/Open360Reframe.r"
  RC_FILE "${CMAKE_CURRENT_SOURCE_DIR}/Open360Reframe.rc"
  OUT_VAR OSV_REFRAME_PIPL_RCP
  DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/ReframeParams.h")

# -----------------------------------------------------------------------------
#  4. Delay loading
#
#  The effect itself needs only nvcuda.dll, but it links osv_premiere_common,
#  which pulls osv_render and with it the library's FFmpeg / OpenCL users.
#  Delay-loading every one of them means:
#    * a machine with no OpenCL ICD still loads the effect;
#    * the vcpkg FFmpeg beside the plug-in wins over the older copy Premiere
#      ships in its own application directory (the DelayLoad hook resolves
#      from the module's folder first);
#    * nvcuda.dll is resolved by the system, not by us, but delay-loading it
#      too means a driver-less machine gets a clean "no GPU" instead of a
#      failure to load the module at all.
#
#  The names must match the DLL file names exactly.  They are read from the
#  imported targets where CMake knows them and spelled out where it does not
#  (the FFmpeg import libraries are plain .lib paths).
# -----------------------------------------------------------------------------
set(_osv_reframe_delayload nvcuda.dll)
if(OSV_ENABLE_OPENCL)
  list(APPEND _osv_reframe_delayload OpenCL.dll)
endif()
foreach(_ff IN ITEMS avcodec avformat avutil swresample swscale)
  # vcpkg's FFmpeg DLLs carry the major version in the file name; find the
  # one that is actually installed rather than guessing the number.
  file(GLOB _ff_dlls "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/${_ff}-*.dll")
  foreach(_ff_dll IN LISTS _ff_dlls)
    get_filename_component(_ff_name "${_ff_dll}" NAME)
    list(APPEND _osv_reframe_delayload "${_ff_name}")
  endforeach()
endforeach()
#  PluginLog goes through spdlog/fmt, so those two are imported as well and
#  must be delay-loaded for the same reason as the rest: Adobe ships its own
#  copies of common libraries in the application directory, which Windows
#  searches before the plug-in's own folder.  A Debug/RelWithDebInfo link
#  surfaces them where a Release link may not, so they are listed explicitly
#  rather than inferred from the configuration.
list(APPEND _osv_reframe_delayload fmt.dll spdlog.dll)
list(REMOVE_DUPLICATES _osv_reframe_delayload)
osv_add_delayload(Open360Reframe DLL_NAMES ${_osv_reframe_delayload})

message(STATUS "Open360Reframe")
message(STATUS "  Match name    : OpenOSV.Open360Reframe")
message(STATUS "  Kernel archs  : ${_osv_reframe_arch_report}")
message(STATUS "  Delay-loaded  : ${_osv_reframe_delayload}")
