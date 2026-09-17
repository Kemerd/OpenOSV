# =============================================================================
#  OsvCheckDelayLoad.cmake  (script mode, run from a POST_BUILD step)
#
#  Asserts that a built plug-in module delay-loads every non-system DLL it
#  depends on.  Why this matters: Premiere Pro's application directory ships
#  its own FFmpeg and CUDA runtime, and the Windows loader searches the
#  application directory before the plug-in's own folder.  A DLL that is
#  imported directly (rather than delay-loaded and resolved by the hook in
#  plugins/common/DelayLoad.cpp) therefore binds to Adobe's copy, and if the
#  major versions ever coincide every call lands in the wrong ABI.
#
#  The check is cheap and runs after every link, so adding a new library that
#  drags in a DLL fails the build with a message naming the DLL instead of
#  producing a module that misbehaves only inside Premiere.
#
#  Inputs (all -D on the cmake -P command line):
#    MODULE              full path of the built module
#    DUMPBIN             full path of dumpbin.exe
#    EXPECTED_DELAYLOAD  '?' separated list of DLLs that must be delay-loaded
#                        ('?' rather than '|', which cmd.exe treats as a pipe
#                        inside the nested POST_BUILD quoting)
#
#  System DLLs (anything under the Windows directory, plus the CRT and the
#  well-known api-ms-* sets) are expected to be direct imports and ignored.
# =============================================================================

if(NOT MODULE OR NOT EXISTS "${MODULE}")
  message(FATAL_ERROR "OsvCheckDelayLoad: MODULE '${MODULE}' does not exist")
endif()
if(NOT DUMPBIN OR NOT EXISTS "${DUMPBIN}")
  # No dumpbin means no check; the configure step already warned about it.
  return()
endif()

string(REPLACE "?" ";" _expected "${EXPECTED_DELAYLOAD}")

execute_process(
  COMMAND "${DUMPBIN}" /DEPENDENTS "${MODULE}"
  OUTPUT_VARIABLE _output
  ERROR_VARIABLE  _error
  RESULT_VARIABLE _result
  OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _result EQUAL 0)
  message(WARNING "OsvCheckDelayLoad: dumpbin failed (${_result}): ${_error}")
  return()
endif()

# dumpbin prints two lists:
#
#   Image has the following dependencies:
#       a.dll
#   Image has the following delay load dependencies:
#       b.dll
#
# Walk the lines and record which section each DLL name appeared in.
string(REPLACE "\n" ";" _lines "${_output}")
set(_section "none")
set(_direct "")
set(_delayed "")
foreach(_line IN LISTS _lines)
  string(STRIP "${_line}" _stripped)
  if(_stripped MATCHES "delay load dependencies")
    set(_section "delayed")
    continue()
  endif()
  if(_stripped MATCHES "following dependencies")
    set(_section "direct")
    continue()
  endif()
  if(_stripped MATCHES "^Summary")
    set(_section "none")
    continue()
  endif()
  if(_stripped MATCHES "^[A-Za-z0-9_.+-]+\\.[Dd][Ll][Ll]$")
    if(_section STREQUAL "direct")
      list(APPEND _direct "${_stripped}")
    elseif(_section STREQUAL "delayed")
      list(APPEND _delayed "${_stripped}")
    endif()
  endif()
endforeach()

# DLLs that are supposed to be direct imports: the OS and the CRT.  Everything
# else must come from beside the plug-in.
set(_system_patterns
  "^api-ms-"
  "^ext-ms-"
  "^KERNEL32\\.dll$"
  "^USER32\\.dll$"
  "^GDI32\\.dll$"
  "^SHELL32\\.dll$"
  "^ADVAPI32\\.dll$"
  "^ole32\\.dll$"
  "^OLEAUT32\\.dll$"
  "^COMCTL32\\.dll$"
  "^COMDLG32\\.dll$"
  "^WS2_32\\.dll$"
  "^SHLWAPI\\.dll$"
  "^bcrypt\\.dll$"
  "^CRYPT32\\.dll$"
  "^Secur32\\.dll$"
  "^MSVCP[0-9]+\\.dll$"
  "^VCRUNTIME[0-9]+.*\\.dll$"
  "^ucrtbase\\.dll$"
  "^nvcuda\\.dll$"       # the CUDA driver; installed with the display driver
  "^cudart64_[0-9]+\\.dll$"
  "^dxgi\\.dll$"
  "^d3d11\\.dll$"
  "^mfplat\\.dll$"
)

set(_unexpected "")
foreach(_dll IN LISTS _direct)
  set(_is_system FALSE)
  foreach(_pattern IN LISTS _system_patterns)
    if(_dll MATCHES "${_pattern}")
      set(_is_system TRUE)
      break()
    endif()
  endforeach()
  if(NOT _is_system)
    list(APPEND _unexpected "${_dll}")
  endif()
endforeach()

get_filename_component(_module_name "${MODULE}" NAME)
if(_unexpected)
  string(REPLACE ";" ", " _unexpected_text "${_unexpected}")
  message(FATAL_ERROR
    "${_module_name} imports these DLLs directly instead of delay-loading them:\n"
    "    ${_unexpected_text}\n"
    "Premiere Pro's application directory is searched before the plug-in's own folder, so a direct import can bind "
    "to Adobe's copy of the same DLL. Add each name to the osv_add_delayload() call in the plug-in's CMakeLists.txt "
    "(or to the system list in cmake/OsvCheckDelayLoad.cmake if it really is an OS component).")
endif()

# Report entries that were declared /DELAYLOAD but are not actually imported.
# Harmless (the linker ignores them), but it means the list has drifted from
# what the module needs, so it is worth one line of build output.
set(_stale "")
foreach(_dll IN LISTS _expected)
  set(_found FALSE)
  foreach(_actual IN LISTS _delayed)
    if("${_dll}" STREQUAL "${_actual}")
      set(_found TRUE)
      break()
    endif()
  endforeach()
  if(NOT _found)
    list(APPEND _stale "${_dll}")
  endif()
endforeach()
if(_stale)
  string(REPLACE ";" ", " _stale_text "${_stale}")
  message(STATUS "${_module_name}: declared but unused /DELAYLOAD entries: ${_stale_text}")
endif()

list(LENGTH _delayed _delayed_count)
message(STATUS "${_module_name}: ${_delayed_count} delay-loaded DLL(s), no unexpected direct imports")
