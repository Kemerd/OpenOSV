# =============================================================================
#  EmbedKernel.cmake
#
#  osv_embed_text_file(<out_cpp> <input_file> <symbol>)
#
#  Turns ANY file - text or binary - into a C++ translation unit exposing
#
#      extern const unsigned char <symbol>[];       the raw bytes, NUL terminated
#      extern const size_t        <symbol>_size;    byte count, no terminator
#      extern const char* const   <symbol>_text;    the same bytes as a C string
#
#  Two users today:
#    * the OpenCL backend embeds its kernel sources (which share osv_kernel.h
#      with the CPU and CUDA renderers) and hands <symbol>_text to
#      clCreateProgramWithSource with no file system access at run time;
#    * the Premiere reframe effect embeds the CUDA fatbin nvcc produced and
#      hands <symbol> to cuModuleLoadFatBinary.
#
#  The element type is `unsigned char` precisely because of the second case:
#  a fatbin is full of bytes above 0x7F, which a signed char cannot hold
#  without a narrowing conversion MSVC rejects.  See EmbedKernelScript.cmake.
#
#  The bytes are emitted as a plain hex initialiser list (no string-literal
#  escaping, no per-literal size limits).  Regenerated whenever the input
#  changes.
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
