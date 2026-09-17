# =============================================================================
#  OsvCopyRuntimeDlls.cmake  (cmake -P script, run POST_BUILD by
#  osv_add_premiere_plugin)
#
#  Copies every runtime DLL a built plug-in module needs into the directory
#  next to it.  Two sources are combined:
#
#    EXTRA_DLLS  : the $<TARGET_RUNTIME_DLLS:...> list CMake computes from
#                  imported SHARED targets (fmt, spdlog, ...).
#    dumpbin     : `dumpbin /DEPENDENTS <file>` on the module and, recursively,
#                  on every DLL copied, so avcodec-63.dll brings avutil-61.dll
#                  and swresample-7.dll along even though CMake only knows the
#                  import libraries.  Delay-load imports are listed by dumpbin
#                  too, so OpenCL.dll and the FFmpeg family are covered.
#
#  A dependency is copied only when it exists in one of SEARCH_DIRS; anything
#  else (kernel32.dll, msvcp140.dll, nvcuda.dll, ...) is a system DLL and is
#  left alone.  Copies use copy_if_different so an unchanged DLL does not
#  bump time stamps.
#
#  Arguments (all -D):
#    MODULE       absolute path of the freshly linked .prm / .aex
#    OUT_DIR      destination directory
#    SEARCH_DIRS  '?' separated list of directories holding candidate DLLs
#    EXTRA_DLLS   '?' separated list of DLL paths known to CMake (may be empty)
#
#  '?' is the separator rather than the more obvious ';' (which the shell
#  would split) or '|' (which was used first and turned out to be a cmd.exe
#  metacharacter: when the POST_BUILD command is nested inside another
#  `cmd /C "..."`, the inner shell mis-pairs the quotes around a value that
#  contains '|' and executes everything after it as a new command, producing
#  "'L:' is not recognized as an internal or external command").  '?' is
#  illegal in a Windows path and has no meaning to cmd outside globbing, so
#  it is safe on both counts.
#    DUMPBIN      path of dumpbin.exe (may be empty: recursion disabled)
# =============================================================================
cmake_minimum_required(VERSION 3.28)

if(NOT MODULE OR NOT EXISTS "${MODULE}")
  message(FATAL_ERROR "OsvCopyRuntimeDlls: MODULE '${MODULE}' does not exist")
endif()
if(NOT OUT_DIR)
  message(FATAL_ERROR "OsvCopyRuntimeDlls: OUT_DIR is required")
endif()
file(MAKE_DIRECTORY "${OUT_DIR}")

string(REPLACE "?" ";" _search_dirs "${SEARCH_DIRS}")
string(REPLACE "?" ";" _extra_dlls "${EXTRA_DLLS}")

# ---------------------------------------------------------------------------
#  Locate a DLL by file name in the search directories.  Returns "" when the
#  name is not ours to ship.
# ---------------------------------------------------------------------------
function(_osv_find_dll NAME OUT)
  foreach(_dir IN LISTS _search_dirs)
    if(_dir AND EXISTS "${_dir}/${NAME}")
      set(${OUT} "${_dir}/${NAME}" PARENT_SCOPE)
      return()
    endif()
  endforeach()
  set(${OUT} "" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#  List the DLL names a PE file imports (static + delay-load).  dumpbin prints
#  the names indented on their own lines; everything ending in ".dll" (case
#  insensitive) is taken.
# ---------------------------------------------------------------------------
function(_osv_list_dependents FILE OUT)
  set(${OUT} "" PARENT_SCOPE)
  if(NOT DUMPBIN OR NOT EXISTS "${DUMPBIN}")
    return()
  endif()
  execute_process(
    COMMAND "${DUMPBIN}" /NOLOGO /DEPENDENTS "${FILE}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(WARNING "OsvCopyRuntimeDlls: dumpbin failed on ${FILE}: ${_err}")
    return()
  endif()
  string(REPLACE "\r" "" _out "${_out}")
  string(REPLACE "\n" ";" _lines "${_out}")
  set(_names "")
  foreach(_line IN LISTS _lines)
    string(STRIP "${_line}" _line)
    if(_line MATCHES "^[^ \t]+\\.[Dd][Ll][Ll]$")
      list(APPEND _names "${_line}")
    endif()
  endforeach()
  set(${OUT} "${_names}" PARENT_SCOPE)
endfunction()

# ---------------------------------------------------------------------------
#  Breadth-first walk over the dependency graph.
# ---------------------------------------------------------------------------
set(_queue "${MODULE}")
foreach(_dll IN LISTS _extra_dlls)
  if(_dll AND EXISTS "${_dll}")
    list(APPEND _queue "${_dll}")
  endif()
endforeach()

set(_visited "")
set(_copied "")
while(_queue)
  list(POP_FRONT _queue _file)
  string(TOLOWER "${_file}" _file_key)
  if("${_file_key}" IN_LIST _visited)
    continue()
  endif()
  list(APPEND _visited "${_file_key}")

  # Everything except the module itself is a DLL we ship: copy it.
  if(NOT "${_file}" STREQUAL "${MODULE}")
    get_filename_component(_name "${_file}" NAME)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_file}" "${OUT_DIR}/${_name}"
                    RESULT_VARIABLE _copy_rc)
    if(NOT _copy_rc EQUAL 0)
      message(FATAL_ERROR "OsvCopyRuntimeDlls: could not copy ${_file} to ${OUT_DIR}")
    endif()
    list(APPEND _copied "${_name}")
  endif()

  # Enqueue this file's own dependencies when they live in our search dirs.
  _osv_list_dependents("${_file}" _deps)
  foreach(_dep IN LISTS _deps)
    _osv_find_dll("${_dep}" _dep_path)
    if(_dep_path)
      list(APPEND _queue "${_dep_path}")
    endif()
  endforeach()
endwhile()

list(LENGTH _copied _count)
if(_count GREATER 0)
  list(SORT _copied)
  string(REPLACE ";" ", " _copied_text "${_copied}")
  message(STATUS "Staged ${_count} runtime DLL(s) next to ${MODULE}: ${_copied_text}")
else()
  message(STATUS "No runtime DLLs to stage for ${MODULE}")
endif()

# ---------------------------------------------------------------------------
#  Delay-load audit.
#
#  Run from here rather than from a second POST_BUILD command: chaining two
#  quoted `cmake.exe` invocations inside one `cmd.exe /C "..."` makes cmd
#  strip only the outermost quotes and then try to execute the second path as
#  a command ("'L:' is not recognized"), which fails the link step.  One
#  script invocation, two jobs.
#
#  CHECK_DELAYLOAD is the path of OsvCheckDelayLoad.cmake; when it is empty
#  the audit is skipped (the plug-in declared no delay-load list).
# ---------------------------------------------------------------------------
if(CHECK_DELAYLOAD AND EXISTS "${CHECK_DELAYLOAD}")
  set(EXPECTED_DELAYLOAD "${EXPECTED_DELAYLOAD}")
  include("${CHECK_DELAYLOAD}")
endif()
