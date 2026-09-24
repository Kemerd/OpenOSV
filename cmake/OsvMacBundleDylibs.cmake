# =============================================================================
#  OsvMacBundleDylibs.cmake  (cmake -P script, POST_BUILD of every macOS
#  plug-in bundle - see _osv_add_premiere_bundle in OsvPremiereSdk.cmake)
#
#  Makes a freshly linked bundle self-contained and loadable:
#
#    1. every @rpath/<lib>.dylib the module loads - directly or through
#       another one of them, e.g. libavformat -> libavcodec -> libavutil - is
#       copied from SEARCH_DIRS into Contents/Frameworks as <PREFIX><lib>,
#       and every reference to it is rewritten to the prefixed name;
#    2. each copied dylib gets that prefixed install name, loses any absolute
#       (build machine) RPATH and finds its siblings through @loader_path;
#    3. the module keeps exactly @loader_path/../Frameworks as its RPATH;
#    4. the dylibs and then the bundle are signed ad hoc.
#
#  Why the prefix: dyld matches an @rpath library that is already loaded by
#  its install name.  Premiere, Media Encoder or another plug-in in the same
#  process may load an FFmpeg of their own under the same "libavcodec.62"
#  name, and the module would then silently use that copy - the Mac form of
#  the problem the Windows delay-load hook (plugins/common/DelayLoad.cpp)
#  exists to prevent.  No other image in the world is called
#  "OpenOSV_libavcodec.62.dylib".
#
#  Idempotent: every link rewrites the module with the original names, and a
#  second run over an already processed bundle changes nothing.
#
#  Inputs (all -D):
#    BUNDLE        the .bundle / .plugin directory
#    BINARY        Contents/MacOS/<name> inside it
#    SEARCH_DIRS   '?' separated directories holding the original dylibs
#    PREFIX        prefix of the embedded copies ("OpenOSV_")
#    SIGN          OFF to skip signing (default ON)
# =============================================================================
cmake_minimum_required(VERSION 3.28)

foreach(_required BUNDLE BINARY PREFIX)
  if(NOT ${_required})
    message(FATAL_ERROR "OsvMacBundleDylibs: ${_required} is required")
  endif()
endforeach()
if(NOT EXISTS "${BINARY}")
  message(FATAL_ERROR "OsvMacBundleDylibs: BINARY '${BINARY}' does not exist")
endif()
if(NOT DEFINED SIGN)
  set(SIGN ON)
endif()

find_program(_otool otool)
find_program(_install_name_tool install_name_tool)
find_program(_codesign codesign)
if(NOT _otool OR NOT _install_name_tool)
  message(FATAL_ERROR "OsvMacBundleDylibs: otool / install_name_tool not found (Xcode command line tools)")
endif()

string(REPLACE "?" ";" _search_dirs "${SEARCH_DIRS}")
set(_frameworks "${BUNDLE}/Contents/Frameworks")
file(MAKE_DIRECTORY "${_frameworks}")

# ---------------------------------------------------------------------------
#  Helpers
# ---------------------------------------------------------------------------

# Run a tool and fail loudly: a half-rewritten bundle must not look finished.
function(_osv_run)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  if(NOT _rc EQUAL 0)
    string(REPLACE ";" " " _cmd "${ARGN}")
    message(FATAL_ERROR "OsvMacBundleDylibs: '${_cmd}' failed (${_rc}):\n${_out}${_err}")
  endif()
endfunction()

# The install name of a dylib (otool -D), "" for anything else.
function(_osv_install_id FILE OUT)
  execute_process(COMMAND "${_otool}" -D "${FILE}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_QUIET)
  set(_id "")
  if(_rc EQUAL 0)
    string(REPLACE "\n" ";" _lines "${_out}")
    list(LENGTH _lines _n)
    if(_n GREATER 1)
      list(GET _lines 1 _id)
      string(STRIP "${_id}" _id)
    endif()
  endif()
  set(${OUT} "${_id}" PARENT_SCOPE)
endfunction()

# The @rpath/... names FILE loads (its own id excluded).
function(_osv_rpath_deps FILE OUT)
  _osv_install_id("${FILE}" _own)
  execute_process(COMMAND "${_otool}" -L "${FILE}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_QUIET)
  set(_names "")
  if(_rc EQUAL 0)
    string(REPLACE "\n" ";" _lines "${_out}")
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      if(_line MATCHES "^(@rpath/[^ ]+) \\(")
        set(_path "${CMAKE_MATCH_1}")
        if(NOT _path STREQUAL _own)
          string(REPLACE "@rpath/" "" _name "${_path}")
          list(APPEND _names "${_name}")
        endif()
      endif()
    endforeach()
  endif()
  list(REMOVE_DUPLICATES _names)
  set(${OUT} "${_names}" PARENT_SCOPE)
endfunction()

# The LC_RPATH entries of FILE.
function(_osv_rpaths FILE OUT)
  execute_process(COMMAND "${_otool}" -l "${FILE}" OUTPUT_VARIABLE _out RESULT_VARIABLE _rc ERROR_QUIET)
  set(_paths "")
  if(_rc EQUAL 0)
    string(REPLACE "\n" ";" _lines "${_out}")
    set(_in_rpath FALSE)
    foreach(_line IN LISTS _lines)
      string(STRIP "${_line}" _line)
      if(_line STREQUAL "cmd LC_RPATH")
        set(_in_rpath TRUE)
      elseif(_in_rpath AND _line MATCHES "^path ([^ ]+) \\(offset")
        list(APPEND _paths "${CMAKE_MATCH_1}")
        set(_in_rpath FALSE)
      endif()
    endforeach()
  endif()
  set(${OUT} "${_paths}" PARENT_SCOPE)
endfunction()

# Leave FILE with exactly WANT as its RPATH (absolute ones removed).
function(_osv_fix_rpaths FILE WANT)
  _osv_rpaths("${FILE}" _have)
  set(_found FALSE)
  foreach(_rp IN LISTS _have)
    if(_rp STREQUAL WANT)
      set(_found TRUE)
    elseif(_rp MATCHES "^/")
      _osv_run("${_install_name_tool}" -delete_rpath "${_rp}" "${FILE}")
    endif()
  endforeach()
  if(NOT _found)
    _osv_run("${_install_name_tool}" -add_rpath "${WANT}" "${FILE}")
  endif()
endfunction()

# ---------------------------------------------------------------------------
#  1. Walk the dependency graph from the module.
# ---------------------------------------------------------------------------
set(_queue "${BINARY}")
set(_embedded "")
while(_queue)
  list(POP_FRONT _queue _file)
  _osv_rpath_deps("${_file}" _deps)
  foreach(_dep IN LISTS _deps)
    # The original name, whether this file still references it (fresh link)
    # or already references the prefixed copy (an earlier run).
    string(LENGTH "${PREFIX}" _prefix_len)
    string(SUBSTRING "${_dep}" 0 ${_prefix_len} _head)
    if(_head STREQUAL PREFIX)
      string(SUBSTRING "${_dep}" ${_prefix_len} -1 _original)
    else()
      set(_original "${_dep}")
    endif()
    set(_renamed "${PREFIX}${_original}")
    set(_target "${_frameworks}/${_renamed}")

    # Copy the real file (vcpkg's names may be symbolic links).
    set(_source "")
    foreach(_dir IN LISTS _search_dirs)
      if(_dir AND EXISTS "${_dir}/${_original}")
        file(REAL_PATH "${_dir}/${_original}" _source)
        break()
      endif()
    endforeach()
    if(_source)
      execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_source}" "${_target}"
                      RESULT_VARIABLE _copy_rc)
      if(NOT _copy_rc EQUAL 0)
        message(FATAL_ERROR "OsvMacBundleDylibs: cannot copy ${_source} to ${_target}")
      endif()
      file(CHMOD "${_target}" PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
    elseif(NOT EXISTS "${_target}")
      message(FATAL_ERROR "OsvMacBundleDylibs: ${_file} loads @rpath/${_dep}, which is in none of: ${_search_dirs}")
    endif()

    # Point this file at the prefixed copy.
    if(NOT _dep STREQUAL _renamed)
      _osv_run("${_install_name_tool}" -change "@rpath/${_dep}" "@rpath/${_renamed}" "${_file}")
    endif()

    if(NOT _renamed IN_LIST _embedded)
      list(APPEND _embedded "${_renamed}")
      list(APPEND _queue "${_target}")
    endif()
  endforeach()
endwhile()

# ---------------------------------------------------------------------------
#  2. The copies: their own name, their siblings through @loader_path.
# ---------------------------------------------------------------------------
foreach(_name IN LISTS _embedded)
  set(_lib "${_frameworks}/${_name}")
  _osv_run("${_install_name_tool}" -id "@rpath/${_name}" "${_lib}")
  _osv_fix_rpaths("${_lib}" "@loader_path")
endforeach()

# ---------------------------------------------------------------------------
#  3. The module: only its own Frameworks folder.
# ---------------------------------------------------------------------------
_osv_fix_rpaths("${BINARY}" "@loader_path/../Frameworks")

# ---------------------------------------------------------------------------
#  4. Ad hoc signatures, nested code first.
# ---------------------------------------------------------------------------
if(SIGN)
  if(NOT _codesign)
    message(FATAL_ERROR "OsvMacBundleDylibs: codesign not found; an unsigned bundle does not load on Apple Silicon")
  endif()
  foreach(_name IN LISTS _embedded)
    _osv_run("${_codesign}" --force --sign - --timestamp=none "${_frameworks}/${_name}")
  endforeach()
  _osv_run("${_codesign}" --force --sign - --timestamp=none "${BUNDLE}")
endif()

list(LENGTH _embedded _count)
string(REPLACE ";" ", " _list "${_embedded}")
message(STATUS "Embedded ${_count} shared librar(ies) in ${BUNDLE}: ${_list}")
