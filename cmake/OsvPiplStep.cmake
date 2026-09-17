# =============================================================================
#  OsvPiplStep.cmake  (cmake -P script, one step of the AE PiPL pipeline)
#
#  Used by osv_add_pipl() in OsvPremiereSdk.cmake so that no shell
#  redirection ("cl ... > file") is needed inside add_custom_command.
#
#  STEP=preprocess
#     Runs `cl /nologo /I... /D... /EP INPUT` and stores stdout in OUTPUT.
#     When DEPFILE is given, `/showIncludes` is added and the "Note: including
#     file:" lines cl prints on stderr are turned into a Makefile-style
#     depfile (OUTPUT: dep1 dep2 ...) so the build system re-runs the step
#     when any included header changes.
#       CL        path of cl.exe
#       INPUT     source file (.r or .rrc)
#       OUTPUT    preprocessed result
#       INCLUDES  '|' separated include directories (optional)
#       DEFINES   '|' separated macro names (optional)
#       DEPFILE   depfile path (optional)
#
#  STEP=pipltool
#     Runs `PiPLtool.exe INPUT OUTPUT`.
#       PIPLTOOL  path of PiPLtool.exe
#       INPUT     preprocessed .rr
#       OUTPUT    .rrc
# =============================================================================
cmake_minimum_required(VERSION 3.28)

if(NOT INPUT OR NOT EXISTS "${INPUT}")
  message(FATAL_ERROR "OsvPiplStep: INPUT '${INPUT}' does not exist")
endif()
if(NOT OUTPUT)
  message(FATAL_ERROR "OsvPiplStep: OUTPUT is required")
endif()
get_filename_component(_out_dir "${OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_out_dir}")

# ---------------------------------------------------------------------------
#  Escape a path for a Makefile-style depfile: spaces become "\ ", and the
#  Windows backslashes become forward slashes (Ninja accepts either).
# ---------------------------------------------------------------------------
function(_osv_depfile_escape PATH OUT)
  file(TO_CMAKE_PATH "${PATH}" _p)
  string(REPLACE " " "\\ " _p "${_p}")
  set(${OUT} "${_p}" PARENT_SCOPE)
endfunction()

if(STEP STREQUAL "preprocess")
  if(NOT CL OR NOT EXISTS "${CL}")
    message(FATAL_ERROR "OsvPiplStep: CL '${CL}' does not exist")
  endif()

  set(_args /nologo)
  if(INCLUDES)
    string(REPLACE "|" ";" _includes "${INCLUDES}")
    foreach(_inc IN LISTS _includes)
      if(_inc)
        file(TO_NATIVE_PATH "${_inc}" _inc_native)
        list(APPEND _args "/I${_inc_native}")
      endif()
    endforeach()
  endif()
  if(DEFINES)
    string(REPLACE "|" ";" _defines "${DEFINES}")
    foreach(_def IN LISTS _defines)
      if(_def)
        list(APPEND _args "/D${_def}")
      endif()
    endforeach()
  endif()
  if(DEPFILE)
    list(APPEND _args /showIncludes)
  endif()
  # /EP: preprocess to stdout without #line directives (PiPLtool cannot
  # parse them).  The file is treated as C source regardless of extension.
  file(TO_NATIVE_PATH "${INPUT}" _input_native)
  list(APPEND _args /EP /TC "${_input_native}")

  execute_process(
    COMMAND "${CL}" ${_args}
    OUTPUT_FILE "${OUTPUT}"
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0)
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "OsvPiplStep: cl /EP failed (${_rc}) on ${INPUT}:\n${_err}")
  endif()

  if(DEPFILE)
    # Collect "Note: including file:   <path>" lines.  cl localises the
    # prefix, so match on the file names instead: every line that ends in a
    # readable header path after a colon is taken.
    string(REPLACE "\r" "" _err "${_err}")
    string(REPLACE "\n" ";" _lines "${_err}")
    set(_deps "")
    foreach(_line IN LISTS _lines)
      if(_line MATCHES "^[^:]*: *[^:]*: *(.+\\.[A-Za-z0-9]+)[ \t]*$")
        string(STRIP "${CMAKE_MATCH_1}" _dep)
        if(EXISTS "${_dep}")
          list(APPEND _deps "${_dep}")
        endif()
      endif()
    endforeach()
    list(REMOVE_DUPLICATES _deps)
    _osv_depfile_escape("${OUTPUT}" _out_esc)
    set(_dep_text "${_out_esc}:")
    foreach(_dep IN LISTS _deps)
      _osv_depfile_escape("${_dep}" _dep_esc)
      string(APPEND _dep_text " \\\n  ${_dep_esc}")
    endforeach()
    string(APPEND _dep_text "\n")
    file(WRITE "${DEPFILE}" "${_dep_text}")
  endif()

elseif(STEP STREQUAL "pipltool")
  if(NOT PIPLTOOL OR NOT EXISTS "${PIPLTOOL}")
    message(FATAL_ERROR "OsvPiplStep: PIPLTOOL '${PIPLTOOL}' does not exist")
  endif()
  file(TO_NATIVE_PATH "${INPUT}" _input_native)
  file(TO_NATIVE_PATH "${OUTPUT}" _output_native)
  execute_process(
    COMMAND "${PIPLTOOL}" "${_input_native}" "${_output_native}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc)
  if(NOT _rc EQUAL 0 OR NOT EXISTS "${OUTPUT}")
    file(REMOVE "${OUTPUT}")
    message(FATAL_ERROR "OsvPiplStep: PiPLtool failed (${_rc}) on ${INPUT}:\n${_out}\n${_err}")
  endif()

else()
  message(FATAL_ERROR "OsvPiplStep: unknown STEP '${STEP}' (preprocess|pipltool)")
endif()
