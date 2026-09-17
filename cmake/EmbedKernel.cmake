# =============================================================================
#  EmbedKernel.cmake
#
#  osv_embed_text_file(<out_cpp> <input_file> <symbol>)
#
#  Turns a text file (the OpenCL kernel source, which shares osv_kernel.h with
#  the CPU and CUDA renderers) into a C++ translation unit exposing
#
#      extern const char   <symbol>[];
#      extern const size_t <symbol>_size;
#
#  so the OpenCL backend can call clCreateProgramWithSource at run time with no
#  file system access.  The bytes are emitted as a plain hex initialiser list
#  (no string-literal escaping, no per-literal size limits) and the array is
#  NUL terminated.  Regenerated whenever the input changes.
# =============================================================================
set(OSV_EMBED_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/EmbedKernelScript.cmake")

function(osv_embed_text_file OUT_CPP INPUT_FILE SYMBOL)
  add_custom_command(
    OUTPUT  "${OUT_CPP}"
    COMMAND "${CMAKE_COMMAND}"
            "-DINPUT=${INPUT_FILE}"
            "-DOUTPUT=${OUT_CPP}"
            "-DSYMBOL=${SYMBOL}"
            -P "${OSV_EMBED_SCRIPT}"
    DEPENDS "${INPUT_FILE}" "${OSV_EMBED_SCRIPT}"
    COMMENT "Embedding ${INPUT_FILE} as ${SYMBOL}"
    VERBATIM)
endfunction()
