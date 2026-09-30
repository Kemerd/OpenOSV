# =============================================================================
#  plugins/vegas - the VEGAS Pro Application Extension (C#, .NET Framework 4.8)
#
#    OpenOSV.Vegas.Core    pure logic, no VEGAS reference   (always built)
#    OpenOSV.Vegas.Tests   console test runner, ctest "vegas" (always built)
#    OpenOSV.Vegas         the extension, x64, references ScriptPortal.Vegas
#                          (built when a VEGAS install - or
#                          OSV_VEGAS_SCRIPTPORTAL - provides that DLL)
#
#  Staged for packaging (the contract WP-V-PKG packages from):
#    ${CMAKE_BINARY_DIR}/plugins/vegas/extension/OpenOSV.Vegas.dll
#    ${CMAKE_BINARY_DIR}/plugins/vegas/extension/OpenOSV.Vegas.Core.dll
#
#  The projects are SDK-style and built by the dotnet CLI; CMake only drives
#  it, so Core and the tests build and run on any Windows machine with the
#  dotnet SDK and the .NET Framework 4.8 targeting pack - CI included - with
#  no VEGAS at all.  No NuGet package is referenced.
# =============================================================================

# Added once, whichever CMakeLists reaches it first (plugins/ today; the root
# could add it too, beside plugins/ofx, without a clash).
get_property(_osv_vegas_added GLOBAL PROPERTY OSV_VEGAS_EXTENSION_ADDED)
if(_osv_vegas_added)
  return()
endif()
set_property(GLOBAL PROPERTY OSV_VEGAS_EXTENSION_ADDED ON)

find_program(OSV_DOTNET_EXECUTABLE NAMES dotnet DOC "The dotnet CLI, for the VEGAS extension (plugins/vegas)")
set(_osv_vegas_default OFF)
if(WIN32 AND OSV_DOTNET_EXECUTABLE)
  set(_osv_vegas_default ON)
endif()
option(OSV_BUILD_VEGAS_EXTENSION "Build the VEGAS Pro Application Extension and its tests (needs the dotnet SDK)" ${_osv_vegas_default})

if(NOT OSV_BUILD_VEGAS_EXTENSION)
  message(STATUS "VEGAS extension : off (OSV_BUILD_VEGAS_EXTENSION=OFF)")
  return()
endif()
if(NOT OSV_DOTNET_EXECUTABLE)
  message(STATUS "VEGAS extension : skipped - the dotnet CLI is not on PATH")
  return()
endif()

# -----------------------------------------------------------------------------
#  ScriptPortal.Vegas.dll: the option, then every VEGAS Pro and Boris FX VEGAS
#  install.  The OLDEST found is used, so the build can never come to depend
#  on a member older releases lack (the extension resolves the running
#  VEGAS's own copy at run time: Private=false, SpecificVersion=false).
# -----------------------------------------------------------------------------
set(OSV_VEGAS_SCRIPTPORTAL "" CACHE FILEPATH "ScriptPortal.Vegas.dll to build the VEGAS extension against (empty = search the VEGAS installs)")
set(_osv_scriptportal "")
if(OSV_VEGAS_SCRIPTPORTAL AND EXISTS "${OSV_VEGAS_SCRIPTPORTAL}")
  set(_osv_scriptportal "${OSV_VEGAS_SCRIPTPORTAL}")
else()
  set(_osv_program_roots "C:/Program Files")
  foreach(_var ProgramW6432 ProgramFiles)
    if(DEFINED ENV{${_var}})
      file(TO_CMAKE_PATH "$ENV{${_var}}" _root)
      list(APPEND _osv_program_roots "${_root}")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES _osv_program_roots)
  set(_osv_candidates "")
  foreach(_root IN LISTS _osv_program_roots)
    file(GLOB _found
         "${_root}/VEGAS/VEGAS Pro */ScriptPortal.Vegas.dll"
         "${_root}/BorisFX/Vegas Pro */ScriptPortal.Vegas.dll"
         "${_root}/BorisFX/VEGAS Pro */ScriptPortal.Vegas.dll")
    list(APPEND _osv_candidates ${_found})
  endforeach()
  if(_osv_candidates)
    list(REMOVE_DUPLICATES _osv_candidates)
    list(SORT _osv_candidates COMPARE NATURAL)
    list(GET _osv_candidates 0 _osv_scriptportal)
  endif()
endif()

# -----------------------------------------------------------------------------
#  Paths
# -----------------------------------------------------------------------------
set(OSV_VEGAS_DOTNET_DIR "${CMAKE_CURRENT_BINARY_DIR}/dotnet/")
set(OSV_VEGAS_STAGE_DIR "${CMAKE_BINARY_DIR}/plugins/vegas/extension")
set(_osv_vegas_bin "${OSV_VEGAS_DOTNET_DIR}bin")
set(_osv_vegas_tests_exe "${_osv_vegas_bin}/OpenOSV.Vegas.Tests/Release/net48/OpenOSV.Vegas.Tests.exe")
set(_osv_vegas_ext_out "${_osv_vegas_bin}/OpenOSV.Vegas/Release/net48")

file(GLOB_RECURSE _osv_vegas_core_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Core/*.cs"
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Core/*.csproj")
file(GLOB_RECURSE _osv_vegas_test_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Tests/*.cs"
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Tests/*.csproj"
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Tests/Fixtures/*")
file(GLOB_RECURSE _osv_vegas_ext_sources CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas/*.cs"
     "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas/*.csproj")
set(_osv_vegas_props "${CMAKE_CURRENT_SOURCE_DIR}/Directory.Build.props")
set(_osv_vegas_common_args -c Release -nologo -v minimal "-p:OsvVegasOut=${OSV_VEGAS_DOTNET_DIR}" "-p:Version=${PROJECT_VERSION}")
# The version the assemblies are stamped with, as a file the builds depend
# on: file(CONFIGURE) rewrites it only when the version changes, so a version
# bump rebuilds both assemblies and nothing else does.
set(_osv_vegas_version_stamp "${OSV_VEGAS_DOTNET_DIR}version.txt")
file(CONFIGURE OUTPUT "${_osv_vegas_version_stamp}" CONTENT "${PROJECT_VERSION}\n")

# -----------------------------------------------------------------------------
#  Core + tests (always)
# -----------------------------------------------------------------------------
add_custom_command(
  OUTPUT "${OSV_VEGAS_DOTNET_DIR}core-tests.stamp"
  COMMAND "${OSV_DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas.Tests/OpenOSV.Vegas.Tests.csproj"
          ${_osv_vegas_common_args}
  COMMAND "${CMAKE_COMMAND}" -E touch "${OSV_VEGAS_DOTNET_DIR}core-tests.stamp"
  DEPENDS ${_osv_vegas_core_sources} ${_osv_vegas_test_sources} "${_osv_vegas_props}" "${_osv_vegas_version_stamp}"
  COMMENT "VEGAS: building OpenOSV.Vegas.Core and its tests (dotnet)"
  VERBATIM)
add_custom_target(osv_vegas_core ALL DEPENDS "${OSV_VEGAS_DOTNET_DIR}core-tests.stamp")

if(OSV_BUILD_TESTS)
  set(_osv_vegas_test_args --source-root "${CMAKE_SOURCE_DIR}")
  # The live osvtool tests: probe and audio extraction are CPU only.
  if(TARGET osvtool)
    list(APPEND _osv_vegas_test_args --osvtool "$<TARGET_FILE:osvtool>")
  endif()
  if(OSV_SAMPLE_FILE AND EXISTS "${OSV_SAMPLE_FILE}")
    list(APPEND _osv_vegas_test_args --sample "${OSV_SAMPLE_FILE}")
  endif()
  # The smoke-test script is compiled as VEGAS compiles it (CodeDom, C# 5).
  if(_osv_scriptportal)
    list(APPEND _osv_vegas_test_args --scriptportal "${_osv_scriptportal}")
  endif()
  add_test(NAME vegas.core COMMAND "${_osv_vegas_tests_exe}" ${_osv_vegas_test_args})
  set_tests_properties(vegas.core PROPERTIES LABELS "vegas" TIMEOUT 300)
endif()

# -----------------------------------------------------------------------------
#  The extension (only with ScriptPortal.Vegas.dll)
# -----------------------------------------------------------------------------
if(NOT _osv_scriptportal)
  message(STATUS "VEGAS extension : Core + tests only - no ScriptPortal.Vegas.dll found "
                 "(install VEGAS Pro, or set -DOSV_VEGAS_SCRIPTPORTAL=<path>)")
  return()
endif()

add_custom_command(
  OUTPUT "${OSV_VEGAS_DOTNET_DIR}extension.stamp"
  COMMAND "${OSV_DOTNET_EXECUTABLE}" build "${CMAKE_CURRENT_SOURCE_DIR}/OpenOSV.Vegas/OpenOSV.Vegas.csproj"
          ${_osv_vegas_common_args} "-p:OsvVegasScriptPortal=${_osv_scriptportal}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OSV_VEGAS_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${_osv_vegas_ext_out}/OpenOSV.Vegas.dll"
          "${_osv_vegas_ext_out}/OpenOSV.Vegas.Core.dll"
          "${OSV_VEGAS_STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${OSV_VEGAS_DOTNET_DIR}extension.stamp"
  # After Core + tests: two dotnet builds of Core at once would fight over
  # its obj folder.
  DEPENDS ${_osv_vegas_ext_sources} ${_osv_vegas_core_sources} "${_osv_vegas_props}" "${_osv_vegas_version_stamp}"
          "${OSV_VEGAS_DOTNET_DIR}core-tests.stamp"
  COMMENT "VEGAS: building the OpenOSV.Vegas extension against ${_osv_scriptportal}"
  VERBATIM)
add_custom_target(osv_vegas_extension ALL DEPENDS "${OSV_VEGAS_DOTNET_DIR}extension.stamp")
add_dependencies(osv_vegas_extension osv_vegas_core)

if(OSV_BUILD_TESTS)
  # No local path may reach a shipped binary: the staged DLLs are scanned
  # for the source and build directories and the user's profile path.
  add_test(NAME vegas.hygiene
           COMMAND "${_osv_vegas_tests_exe}" --filter Hygiene --scan-dir "${OSV_VEGAS_STAGE_DIR}"
                   --forbid "${CMAKE_SOURCE_DIR}" --forbid "${CMAKE_BINARY_DIR}")
  set_tests_properties(vegas.hygiene PROPERTIES LABELS "vegas" TIMEOUT 60)
endif()

message(STATUS "VEGAS extension : ON - ScriptPortal ${_osv_scriptportal}; staged to ${OSV_VEGAS_STAGE_DIR}")
