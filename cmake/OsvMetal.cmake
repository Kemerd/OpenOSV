# =============================================================================
#  OsvMetal.cmake  (macOS only; included from the top-level CMakeLists.txt)
#
#  Everything the Metal renderer backend needs from the build:
#
#    * the OBJCXX language (MetalRenderer.mm talks to Metal in Objective-C++);
#    * osv_metal          INTERFACE target: Metal + Foundation frameworks;
#    * OSV_METAL_COMPILER / OSV_METAL_LINKER   xcrun's `metal` and `metallib`
#                         when the Xcode on this machine has the Metal
#                         toolchain (newer Xcodes download it separately);
#    * osv_metal_library(<out_dir> <name> SOURCES <file>...
#                        OUT_SOURCE <var> OUT_METALLIB <var>)
#          Concatenates the sources in order into <out_dir>/<name>.metal -
#          exactly how the OpenCL program is assembled from preamble.cl,
#          ColorMath.h, osv_kernel.h and kernel.cl - and, when the Metal
#          compiler exists, compiles that file ahead of time into
#          <out_dir>/<name>.metallib (fast math off, the same deployment
#          target as the C++ code).  OUT_METALLIB is empty without the
#          compiler; the renderer then compiles the embedded source at run
#          time instead.  Both are regenerated whenever a source changes, so
#          editing osv_kernel.h rebuilds the Metal library like every other
#          backend.
#
#  Why compile ahead of time at all when the source is embedded anyway: it
#  moves the shader compile (a few hundred milliseconds for this kernel) out
#  of the first frame a host asks for, and it makes a Metal syntax error a
#  BUILD failure on any Mac - including CI runners without a usable GPU -
#  instead of a run-time one.
# =============================================================================
include_guard(GLOBAL)

if(NOT APPLE)
  return()
endif()

enable_language(OBJCXX)
set(CMAKE_OBJCXX_STANDARD 20)
set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
set(CMAKE_OBJCXX_EXTENSIONS OFF)

add_library(osv_metal INTERFACE)
target_link_libraries(osv_metal INTERFACE "-framework Metal" "-framework Foundation")

# -----------------------------------------------------------------------------
#  The offline Metal toolchain (optional).
# -----------------------------------------------------------------------------
set(OSV_METAL_COMPILER "" CACHE FILEPATH "xcrun's metal compiler (found automatically; empty = run-time compile only)")
set(OSV_METAL_LINKER "" CACHE FILEPATH "xcrun's metallib tool (found automatically)")
if(NOT OSV_METAL_COMPILER OR NOT OSV_METAL_LINKER)
  execute_process(COMMAND xcrun -sdk macosx -f metal
                  OUTPUT_VARIABLE _osv_metal OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _osv_metal_rc)
  execute_process(COMMAND xcrun -sdk macosx -f metallib
                  OUTPUT_VARIABLE _osv_metallib OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _osv_metallib_rc)
  # xcrun can name a `metal` shim whose toolchain component is not installed;
  # asking it for its version is what proves it can actually compile.
  if(_osv_metal_rc EQUAL 0 AND _osv_metallib_rc EQUAL 0 AND EXISTS "${_osv_metal}" AND EXISTS "${_osv_metallib}")
    execute_process(COMMAND "${_osv_metal}" --version
                    OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE _osv_metal_ver_rc)
    if(_osv_metal_ver_rc EQUAL 0)
      set(OSV_METAL_COMPILER "${_osv_metal}" CACHE FILEPATH "xcrun's metal compiler" FORCE)
      set(OSV_METAL_LINKER "${_osv_metallib}" CACHE FILEPATH "xcrun's metallib tool" FORCE)
    endif()
  endif()
endif()

if(OSV_METAL_COMPILER AND OSV_METAL_LINKER)
  message(STATUS "Metal compiler : ${OSV_METAL_COMPILER} (kernels compiled ahead of time)")
else()
  message(STATUS "Metal compiler : not found - the Metal renderer compiles its embedded source at run time")
endif()

set(OSV_METAL_CONCAT_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/OsvMetalConcat.cmake")

function(osv_metal_library OUT_DIR NAME)
  cmake_parse_arguments(ARG "" "OUT_SOURCE;OUT_METALLIB" "SOURCES" ${ARGN})
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "osv_metal_library(${NAME}): SOURCES is required")
  endif()
  file(MAKE_DIRECTORY "${OUT_DIR}")
  set(_source "${OUT_DIR}/${NAME}.metal")
  string(REPLACE ";" "?" _inputs "${ARG_SOURCES}")
  add_custom_command(
    OUTPUT  "${_source}"
    COMMAND "${CMAKE_COMMAND}" "-DINPUTS=${_inputs}" "-DOUTPUT=${_source}" -P "${OSV_METAL_CONCAT_SCRIPT}"
    DEPENDS ${ARG_SOURCES} "${OSV_METAL_CONCAT_SCRIPT}"
    COMMENT "Assembling the Metal library source ${NAME}.metal"
    VERBATIM)

  set(_metallib "")
  if(OSV_METAL_COMPILER AND OSV_METAL_LINKER)
    set(_air "${OUT_DIR}/${NAME}.air")
    set(_metallib "${OUT_DIR}/${NAME}.metallib")
    set(_min_os "13.3")
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
      set(_min_os "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    endif()
    # -fno-fast-math: IEEE division / sqrt and no reassociation, the policy
    # every backend follows (see preamble.metal).
    add_custom_command(
      OUTPUT  "${_metallib}"
      COMMAND "${OSV_METAL_COMPILER}" -std=macos-metal2.4 -fno-fast-math "-mmacosx-version-min=${_min_os}"
              -c "${_source}" -o "${_air}"
      COMMAND "${OSV_METAL_LINKER}" "${_air}" -o "${_metallib}"
      DEPENDS "${_source}"
      COMMENT "Compiling the Metal library ${NAME}.metallib"
      VERBATIM)
  endif()

  set(${ARG_OUT_SOURCE} "${_source}" PARENT_SCOPE)
  if(ARG_OUT_METALLIB)
    set(${ARG_OUT_METALLIB} "${_metallib}" PARENT_SCOPE)
  endif()
endfunction()
