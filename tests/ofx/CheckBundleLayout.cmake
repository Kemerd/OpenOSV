# =============================================================================
#  CheckBundleLayout.cmake  (cmake -P, run by the ctest "ofx_bundle_layout")
#
#  Proves the STAGED OpenOSV.ofx.bundle has the layout VEGAS Pro's plug-in
#  scan needs.  VEGAS loads every *.dll and *.ofx under a bundle's
#  Contents/Win64 (subfolders included), so:
#
#    Contents/Win64/OpenOSV.ofx           must be the ONLY file there;
#    Contents/Libraries/Win64/            must hold osvtool.exe and every DLL
#                                         that OpenOSV.ofx and osvtool.exe
#                                         import, directly, delay-loaded or
#                                         through another DLL (walked here
#                                         with dumpbin, like the staging step).
#
#  Inputs (-D):
#    BUNDLE_DIR  the OpenOSV.ofx.bundle folder of the build tree
#    DUMPBIN     dumpbin.exe
# =============================================================================
cmake_minimum_required(VERSION 3.28)

if(NOT BUNDLE_DIR OR NOT IS_DIRECTORY "${BUNDLE_DIR}")
  message(FATAL_ERROR "CheckBundleLayout: BUNDLE_DIR '${BUNDLE_DIR}' is not a directory")
endif()
if(NOT DUMPBIN OR NOT EXISTS "${DUMPBIN}")
  message(FATAL_ERROR "CheckBundleLayout: DUMPBIN '${DUMPBIN}' does not exist")
endif()

set(_win64 "${BUNDLE_DIR}/Contents/Win64")
set(_libs  "${BUNDLE_DIR}/Contents/Libraries/Win64")

# ---------------------------------------------------------------------------
#  1. Contents/Win64 holds exactly OpenOSV.ofx (recursively: a subfolder
#     with a DLL in it is scanned just the same).
# ---------------------------------------------------------------------------
file(GLOB_RECURSE _in_win64 LIST_DIRECTORIES false RELATIVE "${_win64}" "${_win64}/*")
if(NOT "${_in_win64}" STREQUAL "OpenOSV.ofx")
  string(REPLACE ";" ", " _text "${_in_win64}")
  message(FATAL_ERROR "Contents/Win64 must hold exactly OpenOSV.ofx (VEGAS Pro loads everything there); it holds: ${_text}")
endif()

# ---------------------------------------------------------------------------
#  2. osvtool.exe is in Libraries/Win64.
# ---------------------------------------------------------------------------
if(NOT EXISTS "${_libs}/osvtool.exe")
  message(FATAL_ERROR "Contents/Libraries/Win64 has no osvtool.exe (${_libs})")
endif()

# ---------------------------------------------------------------------------
#  3. The import closure of both binaries is in Libraries/Win64.  A name is
#     satisfied by the folder itself, or by being Windows' own (API sets,
#     System32) or the NVIDIA driver's (nvcuda.dll, which is delay-loaded).
# ---------------------------------------------------------------------------
set(_system32 "$ENV{SystemRoot}/System32")

function(_osv_dependents FILE OUT)
  execute_process(
    COMMAND "${DUMPBIN}" /NOLOGO /DEPENDENTS "${FILE}"
    OUTPUT_VARIABLE _out
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    message(FATAL_ERROR "dumpbin failed on ${FILE}")
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

set(_queue "${_win64}/OpenOSV.ofx" "${_libs}/osvtool.exe")
set(_visited "")
set(_missing "")
while(_queue)
  list(POP_FRONT _queue _file)
  string(TOLOWER "${_file}" _key)
  if("${_key}" IN_LIST _visited)
    continue()
  endif()
  list(APPEND _visited "${_key}")

  _osv_dependents("${_file}" _deps)
  foreach(_dep IN LISTS _deps)
    if(EXISTS "${_libs}/${_dep}")
      list(APPEND _queue "${_libs}/${_dep}")
    elseif(_dep MATCHES "^(api|ext)-ms-" OR EXISTS "${_system32}/${_dep}" OR _dep STREQUAL "nvcuda.dll")
      # Windows' own or the display driver's.
    else()
      get_filename_component(_who "${_file}" NAME)
      list(APPEND _missing "${_dep} (imported by ${_who})")
    endif()
  endforeach()
endwhile()

if(_missing)
  list(REMOVE_DUPLICATES _missing)
  string(REPLACE ";" "\n    " _text "${_missing}")
  message(FATAL_ERROR "Contents/Libraries/Win64 lacks DLLs the bundle imports:\n    ${_text}")
endif()

list(LENGTH _visited _count)
message(STATUS "bundle layout OK: Win64 holds OpenOSV.ofx only; Libraries/Win64 covers the ${_count} binaries of the import closure")
