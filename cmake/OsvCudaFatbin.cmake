# =============================================================================
#  OsvCudaFatbin.cmake
#
#  osv_add_cuda_fatbin(<target_name>
#                      SOURCE   <kernel.cu>
#                      SYMBOL   <C++ symbol>
#                      OUT_CPP  <var receiving the generated .cpp path>
#                      [INCLUDES <dir>...]
#                      [DEPENDS  <file>...]
#                      [REPORT   <var receiving the architecture list>])
#
#  Compiles one .cu to a FATBIN with nvcc and embeds it as a byte array
#  (cmake/EmbedKernel.cmake), for a plug-in that loads its kernel with the
#  CUDA DRIVER API (cuModuleLoadFatBinary) into whatever context its host has
#  current - so the plug-in imports nothing from the CUDA runtime and still
#  loads on a machine without CUDA.
#
#  This is the recipe plugins/reframe/CMakeLists.txt uses for the Premiere
#  effect's kernel, as a function, for the OpenFX module (plugins/ofx):
#
#    -fatbin -O3 -fmad=false   no fused multiply-add, so the GPU result stays
#                              inside the parity budget the tests assert
#    real cubins for every architecture the installed nvcc accepts (probed
#    once at configure time: a CUDA 13 toolkit drops sm_50..sm_70, a 12.0 one
#    has no sm_120) plus compute_50 PTX, which the driver JITs for anything
#    newer we built no cubin for.
#
#  The generated source defines, at global scope with C++ linkage:
#      extern const unsigned char <SYMBOL>[];
#      extern const std::size_t   <SYMBOL>_size;
# =============================================================================
include("${CMAKE_CURRENT_LIST_DIR}/EmbedKernel.cmake")

function(osv_add_cuda_fatbin NAME)
  set(_options)
  set(_one SOURCE SYMBOL OUT_CPP REPORT)
  set(_multi INCLUDES DEPENDS)
  cmake_parse_arguments(ARG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(NOT ARG_SOURCE OR NOT ARG_SYMBOL OR NOT ARG_OUT_CPP)
    message(FATAL_ERROR "osv_add_cuda_fatbin(${NAME}): SOURCE, SYMBOL and OUT_CPP are required")
  endif()
  if(NOT CMAKE_CUDA_COMPILER)
    message(FATAL_ERROR "osv_add_cuda_fatbin(${NAME}): no CUDA compiler (CMAKE_CUDA_COMPILER is empty)")
  endif()

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/generated/${NAME}")
  file(MAKE_DIRECTORY "${_gen_dir}")
  set(_fatbin "${_gen_dir}/${NAME}.fatbin")
  set(_cpp    "${_gen_dir}/${NAME}Fatbin.cpp")

  # ---- which real architectures can this nvcc emit? --------------------------
  set(_candidates 60 61 70 75 80 86 89 90 120)
  set(_gencode "")
  set(_report "")
  set(_probe_src "${_gen_dir}/arch_probe.cu")
  file(WRITE "${_probe_src}" "__global__ void osvArchProbe() {}\n")
  foreach(_arch IN LISTS _candidates)
    execute_process(
      COMMAND "${CMAKE_CUDA_COMPILER}" -fatbin -Wno-deprecated-gpu-targets
              "-gencode" "arch=compute_${_arch},code=sm_${_arch}"
              -o "${_gen_dir}/arch_probe_${_arch}.fatbin" "${_probe_src}"
      RESULT_VARIABLE _rc OUTPUT_QUIET ERROR_QUIET)
    if(_rc EQUAL 0)
      list(APPEND _gencode "-gencode" "arch=compute_${_arch},code=sm_${_arch}")
      list(APPEND _report "sm_${_arch}")
    endif()
  endforeach()
  if(NOT _gencode)
    message(FATAL_ERROR "osv_add_cuda_fatbin(${NAME}): nvcc accepted none of ${_candidates}")
  endif()

  # PTX for compute_50, so a device we built no cubin for still runs.
  execute_process(
    COMMAND "${CMAKE_CUDA_COMPILER}" -fatbin -Wno-deprecated-gpu-targets
            "-gencode" "arch=compute_50,code=compute_50"
            -o "${_gen_dir}/ptx_probe.fatbin" "${_probe_src}"
    RESULT_VARIABLE _ptx_rc OUTPUT_QUIET ERROR_QUIET)
  if(_ptx_rc EQUAL 0)
    list(INSERT _gencode 0 "-gencode" "arch=compute_50,code=compute_50")
    list(INSERT _report 0 "compute_50 (PTX)")
  endif()

  set(_include_flags "")
  foreach(_dir IN LISTS ARG_INCLUDES)
    list(APPEND _include_flags "-I${_dir}")
  endforeach()

  add_custom_command(
    OUTPUT  "${_fatbin}"
    COMMAND "${CMAKE_CUDA_COMPILER}"
            -fatbin -O3 -fmad=false -Wno-deprecated-gpu-targets
            ${_gencode}
            ${_include_flags}
            -o "${_fatbin}" "${ARG_SOURCE}"
    DEPENDS "${ARG_SOURCE}" ${ARG_DEPENDS}
    COMMENT "nvcc ${NAME} -> fatbin (${_report})"
    VERBATIM)

  osv_embed_text_file("${_cpp}" "${_fatbin}" "${ARG_SYMBOL}")
  set_source_files_properties("${_cpp}" PROPERTIES GENERATED TRUE)

  set(${ARG_OUT_CPP} "${_cpp}" PARENT_SCOPE)
  if(ARG_REPORT)
    set(${ARG_REPORT} "${_report}" PARENT_SCOPE)
  endif()
endfunction()
