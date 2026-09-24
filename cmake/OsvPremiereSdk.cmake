# =============================================================================
#  OsvPremiereSdk.cmake
#
#  Adobe SDK discovery, validation and the helper functions the Premiere Pro
#  plug-in targets are built with (see docs/PREMIERE.md, "Build").
#
#  The Adobe Premiere Pro and After Effects SDK headers are licensed for use
#  but NOT for redistribution, so this repository never contains them.  A
#  developer downloads both SDKs with an Adobe ID and points two cache
#  variables at the extracted folders:
#
#     -DOSV_BUILD_PREMIERE=ON
#     -DOSV_PREMIERE_SDK_DIR="C:/Adobe/Premiere Pro 26.0 C++ SDK"
#     -DOSV_AE_SDK_DIR="C:/Adobe/AfterEffectsSDK"
#
#  The variables are validated only when the plug-ins are requested so that a
#  plain library build never depends on Adobe material.
#
#  What this module defines (only when OSV_BUILD_PREMIERE is ON):
#
#    osv_premiere_sdk   INTERFACE target: Premiere headers + the preprocessor
#                       defines every Adobe sample compiles with.
#    osv_ae_sdk         INTERFACE target: After Effects headers (Headers,
#                       Headers/SP, Util, Resources).  Premiere's header
#                       directory is listed FIRST so that the newer copies of
#                       PrSDKAESupport.h / PrSDKPixelFormat.h shipped with the
#                       Premiere SDK win over the older ones in the AE SDK.
#
#    osv_add_premiere_plugin(<target> KIND prm|aex SOURCES ...
#                            [OUTPUT_DIRECTORY <dir>])
#    osv_add_pipl(<target> R_FILE <file.r> OUT_VAR <var>
#                 [RC_FILE <file.rc>] [DEPENDS ...])
#    osv_add_delayload(<target> DLL_NAMES <a.dll> <b.dll> ...)
#
#  Variables exported for status summaries:
#    OSV_PREMIERE_SDK_IMPORTMOD_VERSION   e.g. "24 (23.2)"
#    OSV_AE_SDK_SPEC_VERSION              e.g. "13.29"
#    OSV_PLUGIN_STAGE_DIR                 cache PATH, where plug-ins are staged
#
#  macOS.  The same three functions build Mac bundles instead (see the
#  APPLE branches below, and docs/BUILDING_MAC.md):
#    * osv_add_premiere_plugin makes a loadable bundle - KIND prm becomes
#      <target>.bundle (package type BNDL, like Adobe's importer samples),
#      KIND aex becomes <target>.plugin (eFKT / FXTC, like the AE samples) -
#      compiled with hidden visibility, and a post-build step copies the
#      FFmpeg dylibs the module loads into Contents/Frameworks under
#      OpenOSV-prefixed install names and signs everything ad hoc
#      (cmake/OsvMacBundleDylibs.cmake);
#    * osv_add_pipl compiles the .r with Rez into Contents/Resources/<target>.rsrc
#      (the After Effects Xcode samples' Rez phase), no PiPLtool involved;
#    * osv_add_delayload has nothing to do: the unique install names are what
#      keeps a bundle from binding to a host's copy of a library.
#  The SDK needs no PiPLtool.exe there, only the headers and AE_General.r.
# =============================================================================
if(NOT OSV_BUILD_PREMIERE)
  return()
endif()

if(APPLE)
  include(OsvMacBundle)
endif()

set(OSV_PREMIERE_SDK_DOWNLOAD_URL "https://developer.adobe.com/console/servicesandapis/pr")
set(OSV_AE_SDK_DOWNLOAD_URL       "https://developer.adobe.com/console/servicesandapis/ae")

# -----------------------------------------------------------------------------
#  Validation of the two SDK roots.  Every file that the build later relies on
#  is checked here so a wrong path fails with one clear message instead of a
#  cryptic compiler error deep inside the plug-in sources.
# -----------------------------------------------------------------------------
function(_osv_require_sdk_file PATH WHAT VARNAME URL)
  if(NOT EXISTS "${PATH}")
    message(FATAL_ERROR
      "OSV_BUILD_PREMIERE=ON but ${WHAT} was not found at:\n"
      "    ${PATH}\n"
      "Set ${VARNAME} to the root of the extracted SDK (the folder that contains "
      "'Examples/Headers').  Download it with an Adobe ID from\n"
      "    ${URL}\n"
      "The SDKs are never committed to this repository (Adobe licence).")
  endif()
endfunction()

if(NOT OSV_PREMIERE_SDK_DIR)
  message(FATAL_ERROR
    "OSV_BUILD_PREMIERE=ON but OSV_PREMIERE_SDK_DIR is empty. Point it at the Premiere Pro C++ SDK root "
    "(download: ${OSV_PREMIERE_SDK_DOWNLOAD_URL}).")
endif()
if(NOT OSV_AE_SDK_DIR)
  message(FATAL_ERROR
    "OSV_BUILD_PREMIERE=ON but OSV_AE_SDK_DIR is empty. Point it at the After Effects C++ SDK root "
    "(download: ${OSV_AE_SDK_DOWNLOAD_URL}).")
endif()

# Normalise to forward slashes so generator expressions and the helper
# scripts never see a stray backslash.
file(TO_CMAKE_PATH "${OSV_PREMIERE_SDK_DIR}" OSV_PREMIERE_SDK_DIR)
file(TO_CMAKE_PATH "${OSV_AE_SDK_DIR}"       OSV_AE_SDK_DIR)

set(OSV_PREMIERE_SDK_HEADERS "${OSV_PREMIERE_SDK_DIR}/Examples/Headers")
set(OSV_AE_SDK_HEADERS       "${OSV_AE_SDK_DIR}/Examples/Headers")
set(OSV_AE_SDK_RESOURCES     "${OSV_AE_SDK_DIR}/Examples/Resources")
set(OSV_AE_SDK_UTIL          "${OSV_AE_SDK_DIR}/Examples/Util")
set(OSV_AE_PIPLTOOL          "${OSV_AE_SDK_RESOURCES}/PiPLtool.exe")

_osv_require_sdk_file("${OSV_PREMIERE_SDK_HEADERS}/PrSDKImport.h"
  "the Premiere Pro SDK (Examples/Headers/PrSDKImport.h)" OSV_PREMIERE_SDK_DIR "${OSV_PREMIERE_SDK_DOWNLOAD_URL}")
_osv_require_sdk_file("${OSV_AE_SDK_HEADERS}/AE_Effect.h"
  "the After Effects SDK (Examples/Headers/AE_Effect.h)" OSV_AE_SDK_DIR "${OSV_AE_SDK_DOWNLOAD_URL}")
# PiPLtool.exe is the Windows half of the PiPL pipeline; a Mac compiles the
# same .r with Rez and never needs it.
if(NOT APPLE)
  _osv_require_sdk_file("${OSV_AE_PIPLTOOL}"
    "the After Effects PiPL tool (Examples/Resources/PiPLtool.exe)" OSV_AE_SDK_DIR "${OSV_AE_SDK_DOWNLOAD_URL}")
endif()
_osv_require_sdk_file("${OSV_AE_SDK_RESOURCES}/AE_General.r"
  "the After Effects PiPL template (Examples/Resources/AE_General.r)" OSV_AE_SDK_DIR "${OSV_AE_SDK_DOWNLOAD_URL}")
_osv_require_sdk_file("${OSV_AE_SDK_UTIL}/AEFX_SuiteHelper.c"
  "the After Effects utility sources (Examples/Util/AEFX_SuiteHelper.c)" OSV_AE_SDK_DIR "${OSV_AE_SDK_DOWNLOAD_URL}")

# -----------------------------------------------------------------------------
#  SDK version strings for the configure summary.
#    Premiere: "#define IMPORTMOD_VERSION IMPORTMOD_VERSION_24" and
#              "#define IMPORTMOD_VERSION_24 24 // 23.2"
#    AE:       "#define PF_PLUG_IN_VERSION 13" / "#define PF_PLUG_IN_SUBVERS 29"
# -----------------------------------------------------------------------------
set(OSV_PREMIERE_SDK_IMPORTMOD_VERSION "unknown")
file(STRINGS "${OSV_PREMIERE_SDK_HEADERS}/PrSDKImport.h" _osv_importmod_lines
     REGEX "^#define[ \t]+IMPORTMOD_VERSION[ \t]")
if(_osv_importmod_lines)
  list(GET _osv_importmod_lines 0 _osv_importmod_line)
  string(REGEX REPLACE "^#define[ \t]+IMPORTMOD_VERSION[ \t]+([A-Za-z0-9_]+).*$" "\\1"
         _osv_importmod_alias "${_osv_importmod_line}")
  file(STRINGS "${OSV_PREMIERE_SDK_HEADERS}/PrSDKImport.h" _osv_importmod_value_lines
       REGEX "^#define[ \t]+${_osv_importmod_alias}[ \t]")
  if(_osv_importmod_value_lines)
    list(GET _osv_importmod_value_lines 0 _osv_importmod_value_line)
    string(REGEX REPLACE "^#define[ \t]+${_osv_importmod_alias}[ \t]+([0-9]+)[ \t]*(//[ \t]*([^ \t]*))?.*$" "\\1"
           _osv_importmod_number "${_osv_importmod_value_line}")
    string(REGEX MATCH "//[ \t]*([0-9.]+)" _osv_importmod_comment "${_osv_importmod_value_line}")
    if(CMAKE_MATCH_1)
      set(OSV_PREMIERE_SDK_IMPORTMOD_VERSION "${_osv_importmod_number} (Premiere ${CMAKE_MATCH_1})")
    else()
      set(OSV_PREMIERE_SDK_IMPORTMOD_VERSION "${_osv_importmod_number}")
    endif()
  endif()
endif()

set(OSV_AE_SDK_SPEC_VERSION "unknown")
if(EXISTS "${OSV_AE_SDK_HEADERS}/AE_EffectVers.h")
  file(STRINGS "${OSV_AE_SDK_HEADERS}/AE_EffectVers.h" _osv_ae_ver_line REGEX "^#define[ \t]+PF_PLUG_IN_VERSION[ \t]")
  file(STRINGS "${OSV_AE_SDK_HEADERS}/AE_EffectVers.h" _osv_ae_sub_line REGEX "^#define[ \t]+PF_PLUG_IN_SUBVERS[ \t]")
  if(_osv_ae_ver_line AND _osv_ae_sub_line)
    string(REGEX REPLACE "^#define[ \t]+PF_PLUG_IN_VERSION[ \t]+([0-9]+).*$" "\\1" _osv_ae_ver "${_osv_ae_ver_line}")
    string(REGEX REPLACE "^#define[ \t]+PF_PLUG_IN_SUBVERS[ \t]+([0-9]+).*$" "\\1" _osv_ae_sub "${_osv_ae_sub_line}")
    set(OSV_AE_SDK_SPEC_VERSION "${_osv_ae_ver}.${_osv_ae_sub}")
  endif()
endif()

# -----------------------------------------------------------------------------
#  Stage directory: where built plug-ins and their runtime DLLs are collected.
#  scripts/install_plugins.ps1 copies this folder into MediaCore\OpenOSV.
# -----------------------------------------------------------------------------
set(OSV_PLUGIN_STAGE_DIR "${CMAKE_BINARY_DIR}/plugins/OpenOSV"
    CACHE PATH "Directory where the built Premiere plug-ins and their runtime DLLs are staged")
file(TO_CMAKE_PATH "${OSV_PLUGIN_STAGE_DIR}" OSV_PLUGIN_STAGE_DIR)
file(MAKE_DIRECTORY "${OSV_PLUGIN_STAGE_DIR}")

# -----------------------------------------------------------------------------
#  Tools used by the helper functions.
#    dumpbin  : lists the DLL imports of a built module so the post-build step
#               can copy exactly the runtime DLLs the plug-in needs.
#    cl       : the preprocessor for the PiPL pipeline (must be the MSVC cl).
# -----------------------------------------------------------------------------
if(APPLE)
  # macOS: Rez compiles the PiPL (osv_add_pipl), and the bundle post-build
  # step uses otool / install_name_tool / codesign, which ship with the
  # command line tools and are looked up on PATH by that script.
  execute_process(COMMAND xcrun -f Rez OUTPUT_VARIABLE _osv_rez OUTPUT_STRIP_TRAILING_WHITESPACE
                  ERROR_QUIET RESULT_VARIABLE _osv_rez_rc)
  if(_osv_rez_rc EQUAL 0 AND EXISTS "${_osv_rez}")
    set(OSV_REZ_EXE "${_osv_rez}" CACHE FILEPATH "Apple's Rez resource compiler (PiPL)")
  else()
    find_program(OSV_REZ_EXE NAMES Rez HINTS /usr/bin DOC "Apple's Rez resource compiler (PiPL)")
  endif()
  if(NOT OSV_REZ_EXE)
    message(FATAL_ERROR "Rez was not found; the PiPL resources need it. Install the Xcode command line tools "
                        "(xcode-select --install).")
  endif()
  execute_process(COMMAND xcrun --show-sdk-path OUTPUT_VARIABLE OSV_MACOS_SDK_PATH
                  OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
else()
  get_filename_component(_osv_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(OSV_DUMPBIN_EXE NAMES dumpbin HINTS "${_osv_cl_dir}" DOC "MSVC dumpbin.exe (runtime DLL discovery)")
  find_program(OSV_MSVC_CL_EXE NAMES cl HINTS "${_osv_cl_dir}" DOC "MSVC cl.exe (PiPL preprocessing)")
  if(NOT OSV_DUMPBIN_EXE)
    message(WARNING
      "dumpbin.exe was not found; the plug-in post-build step will only copy DLLs known to CMake "
      "(imported SHARED targets) and not the FFmpeg/OpenCL DLLs. Configure from a Visual Studio developer shell.")
  endif()
  if(NOT OSV_MSVC_CL_EXE)
    message(FATAL_ERROR "cl.exe was not found next to the C++ compiler; the PiPL pipeline needs the MSVC preprocessor.")
  endif()
endif()

set(OSV_COPY_RUNTIME_DLLS_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/OsvCopyRuntimeDlls.cmake")
set(OSV_CHECK_DELAYLOAD_SCRIPT   "${CMAKE_CURRENT_LIST_DIR}/OsvCheckDelayLoad.cmake")
set(OSV_PIPL_STEP_SCRIPT         "${CMAKE_CURRENT_LIST_DIR}/OsvPiplStep.cmake")

# Directories searched for runtime DLLs by the post-build copy step.  The
# vcpkg bin folder holds FFmpeg, OpenCL.dll, fmt, spdlog, zlib, miniz.
set(_osv_dll_search_dirs "")
if(APPLE AND VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  # macOS keeps shared libraries (the FFmpeg dylibs) in lib/, not bin/.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _osv_dll_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
  endif()
  list(APPEND _osv_dll_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
elseif(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  list(APPEND _osv_dll_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    list(APPEND _osv_dll_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
  endif()
elseif(EXISTS "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin")
  list(APPEND _osv_dll_search_dirs "${CMAKE_SOURCE_DIR}/vcpkg_installed/x64-windows/bin")
endif()
list(APPEND _osv_dll_search_dirs "${CMAKE_BINARY_DIR}/bin")
set(OSV_PLUGIN_DLL_SEARCH_DIRS "${_osv_dll_search_dirs}" CACHE STRING
    "Directories searched for the runtime DLLs copied next to the plug-ins")

# -----------------------------------------------------------------------------
#  INTERFACE targets
# -----------------------------------------------------------------------------
add_library(osv_premiere_sdk INTERFACE)
target_include_directories(osv_premiere_sdk INTERFACE "${OSV_PREMIERE_SDK_HEADERS}")
# PRWIN_ENV / MSWindows select the Windows branch of every Adobe header and
# _WINDOWS / _USE_MATH_DEFINES are what the sample projects define.
#
# NOMINMAX is deliberately NOT defined here: osv_warnings already puts it on
# the command line for the whole project.
#
# macOS needs none of them: PrSDKTypes.h includes PrSDKSetEnv.h, which
# defines PRMAC_ENV from the compiler's own __APPLE__, and AEConfig.h does
# the same for AE_OS_MAC.
if(NOT APPLE)
  target_compile_definitions(osv_premiere_sdk INTERFACE
    PRWIN_ENV
    MSWindows
    _WINDOWS
    _USE_MATH_DEFINES
  )
endif()

# PrSDKTypes.h line 41 does an unconditional `#define NOMINMAX` inside its
# `#if defined(PRWIN_ENV)` branch.  Because osv_warnings defines the very same
# macro on the command line (identically, to 1), every translation unit that
# reaches an Adobe header raises C4005 "macro redefinition" - which is fatal
# under OSV_WARNINGS_AS_ERRORS.  The macro cannot be dropped from either side
# (the project needs it for <windows.h>, the SDK header sets it before pulling
# <windows.h> in itself), so C4005 is disabled for exactly the targets that
# include Adobe headers.  Everything that does not link this target keeps the
# warning.
#
# C4505 comes from the same place: PrSDKPlayModule.h:613 defines
# ConvertToPmDisplayStateProperties2() as a static inline helper that most
# translation units never call, and /W4 reports the removed function against
# OUR file even though the code is Adobe's.
if(MSVC)
  target_compile_options(osv_premiere_sdk INTERFACE
    $<$<COMPILE_LANGUAGE:CXX>:/wd4005;/wd4505>
    $<$<COMPILE_LANGUAGE:C>:/wd4005;/wd4505>
  )
endif()

add_library(osv_ae_sdk INTERFACE)
# Premiere's header directory comes first on purpose (see file header).
set(_osv_ae_includes "${OSV_PREMIERE_SDK_HEADERS}" "${OSV_AE_SDK_HEADERS}")
if(EXISTS "${OSV_AE_SDK_HEADERS}/SP")
  list(APPEND _osv_ae_includes "${OSV_AE_SDK_HEADERS}/SP")
endif()
list(APPEND _osv_ae_includes "${OSV_AE_SDK_UTIL}" "${OSV_AE_SDK_RESOURCES}")
target_include_directories(osv_ae_sdk INTERFACE ${_osv_ae_includes})
target_link_libraries(osv_ae_sdk INTERFACE osv_premiere_sdk)

message(STATUS "Premiere SDK : ${OSV_PREMIERE_SDK_DIR} (IMPORTMOD_VERSION ${OSV_PREMIERE_SDK_IMPORTMOD_VERSION})")
message(STATUS "AE SDK       : ${OSV_AE_SDK_DIR} (effect spec ${OSV_AE_SDK_SPEC_VERSION})")
message(STATUS "Plug-in stage: ${OSV_PLUGIN_STAGE_DIR}")

# =============================================================================
#  osv_add_premiere_plugin(<target> KIND prm|aex SOURCES <src>...
#                          [OUTPUT_DIRECTORY <dir>] [EXPORTS <c-symbol>...])
#
#  EXPORTS names the module's entry points.  Windows ignores it (the
#  entry points carry __declspec(dllexport)); a macOS bundle exports exactly
#  these and nothing else (cmake/OsvMacBundle.cmake).
#
#  Creates a MODULE library laid out the way Premiere Pro expects a plug-in:
#
#    * file name  <target>.prm (importer) or <target>.aex (AE-API effect),
#      no "lib" prefix;
#    * /MD (or /MDd in Debug) - Adobe's loader shares fiber-local storage
#      slots between plug-ins and a static CRT exhausts them (SDK guide
#      3.10.4), so the dynamic CRT is mandatory;
#    * /SUBSYSTEM:WINDOWS, x64, the project's warning flags (osv_warnings);
#    * linked against osv_premiere_sdk and, for KIND aex, osv_ae_sdk as well
#      (the AE-API effect needs AE_Effect.h and friends);
#    * placed in OSV_PLUGIN_STAGE_DIR (or OUTPUT_DIRECTORY when given) for
#      every configuration, PDB alongside;
#    * a POST_BUILD step that copies the runtime DLLs the module imports
#      (directly or delay-loaded, recursively) into the same directory.  The
#      list comes from $<TARGET_RUNTIME_DLLS:...> for imported SHARED targets
#      and from `dumpbin /DEPENDENTS` on the built module for everything
#      else (the FFmpeg import libraries are plain .lib paths, so CMake does
#      not know their DLL names); only DLLs found in
#      OSV_PLUGIN_DLL_SEARCH_DIRS are copied, system DLLs are skipped.
#
#  The caller adds its own sources, extra libraries (osv_premiere_common,
#  osv_render, ...) and delay-load entries afterwards with the usual
#  target_* commands and osv_add_delayload().
# =============================================================================
# -----------------------------------------------------------------------------
#  _osv_add_premiere_bundle(<target> <kind> <out_dir> <sources>...)   (macOS)
#
#  The Mac shape of a plug-in, following Adobe's Xcode samples, built by
#  osv_add_mac_bundle() (cmake/OsvMacBundle.cmake, which explains the layout,
#  the hidden visibility and the embedded, renamed FFmpeg):
#
#    <out_dir>/<target>.bundle/  KIND prm: package type BNDL, as SDK_File_Import
#    <out_dir>/<target>.plugin/  KIND aex: eFKT / FXTC, as the AE effect samples
#
#  plus the Adobe SDK targets every plug-in compiles against.  The PiPL /
#  IMPT resource is added by osv_add_pipl() into Contents/Resources.
# -----------------------------------------------------------------------------
function(_osv_add_premiere_bundle TARGET KIND OUT_DIR EXPORTS)
  if(KIND STREQUAL "prm")
    osv_add_mac_bundle(${TARGET} EXTENSION bundle PACKAGE_TYPE BNDL SIGNATURE "????"
                       OUTPUT_DIRECTORY "${OUT_DIR}" EXPORTS ${EXPORTS} SOURCES ${ARGN})
  else()
    osv_add_mac_bundle(${TARGET} EXTENSION plugin PACKAGE_TYPE eFKT SIGNATURE FXTC
                       OUTPUT_DIRECTORY "${OUT_DIR}" EXPORTS ${EXPORTS} SOURCES ${ARGN})
  endif()
  set_target_properties(${TARGET} PROPERTIES FOLDER "plugins")
  target_link_libraries(${TARGET} PRIVATE osv_warnings osv_premiere_sdk)
  if(KIND STREQUAL "aex")
    target_link_libraries(${TARGET} PRIVATE osv_ae_sdk)
  endif()
endfunction()

function(osv_add_premiere_plugin TARGET)
  set(_options)
  set(_one KIND OUTPUT_DIRECTORY)
  set(_multi SOURCES EXPORTS)
  cmake_parse_arguments(ARG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(NOT ARG_KIND)
    message(FATAL_ERROR "osv_add_premiere_plugin(${TARGET}): KIND prm|aex is required")
  endif()
  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "osv_add_premiere_plugin(${TARGET}): at least one SOURCES entry is required")
  endif()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "osv_add_premiere_plugin(${TARGET}): unknown arguments '${ARG_UNPARSED_ARGUMENTS}'")
  endif()

  string(TOLOWER "${ARG_KIND}" _kind)
  if(_kind STREQUAL "prm")
    set(_suffix ".prm")
  elseif(_kind STREQUAL "aex")
    set(_suffix ".aex")
  else()
    message(FATAL_ERROR "osv_add_premiere_plugin(${TARGET}): KIND must be prm or aex, got '${ARG_KIND}'")
  endif()

  if(ARG_OUTPUT_DIRECTORY)
    file(TO_CMAKE_PATH "${ARG_OUTPUT_DIRECTORY}" _out_dir)
  else()
    set(_out_dir "${OSV_PLUGIN_STAGE_DIR}")
  endif()
  file(MAKE_DIRECTORY "${_out_dir}")

  # macOS: a loadable bundle instead of a DLL (see the file header).
  if(APPLE)
    if(NOT ARG_EXPORTS)
      message(FATAL_ERROR "osv_add_premiere_plugin(${TARGET}): a macOS bundle needs EXPORTS (its entry points)")
    endif()
    _osv_add_premiere_bundle(${TARGET} "${_kind}" "${_out_dir}" "${ARG_EXPORTS}" ${ARG_SOURCES})
    return()
  endif()

  # Turn vcpkg's own "applocal" deployment off for this module.
  #
  # vcpkg's toolchain overrides add_library() and, when VCPKG_APPLOCAL_DEPS is
  # on, appends `&& cmd /C "cd /D <dir> && vcpkg z-applocal ..."` to the LINK
  # command of every SHARED or MODULE library.  Our own POST_BUILD staging step
  # then lands INSIDE that nested quoted string: the inner cmd strips the
  # quotes around "C:\Program Files\CMake\bin\cmake.exe", the outer string's
  # closing quote is left dangling, and cmd tries to execute `L:` ("'L:' is not
  # recognized as an internal or external command").
  #
  # Turning it off costs nothing, because OsvCopyRuntimeDlls.cmake stages every
  # DLL the built module actually imports - including the FFmpeg and OpenCL
  # ones vcpkg's heuristic does not find - into the same directory.  The
  # variable is read at add_library() time, so it only has to be off here.
  set(_osv_saved_applocal "${VCPKG_APPLOCAL_DEPS}")
  set(VCPKG_APPLOCAL_DEPS OFF)
  add_library(${TARGET} MODULE ${ARG_SOURCES})
  set(VCPKG_APPLOCAL_DEPS "${_osv_saved_applocal}")

  # The $<1:...> wrapper stops multi-config generators from appending a
  # per-configuration sub directory, so Release and Debug builds both land
  # exactly in the stage directory.
  set_target_properties(${TARGET} PROPERTIES
    PREFIX ""
    SUFFIX "${_suffix}"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${_out_dir}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_out_dir}>"
    PDB_OUTPUT_DIRECTORY     "$<1:${_out_dir}>"
    MSVC_RUNTIME_LIBRARY     "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    POSITION_INDEPENDENT_CODE ON
    FOLDER "plugins"
  )

  if(MSVC)
    target_link_options(${TARGET} PRIVATE /SUBSYSTEM:WINDOWS)
  endif()

  target_link_libraries(${TARGET} PRIVATE osv_warnings osv_premiere_sdk)
  if(_kind STREQUAL "aex")
    target_link_libraries(${TARGET} PRIVATE osv_ae_sdk)
  endif()

  # Post-build: copy the runtime DLLs next to the module, then audit the
  # module's imports.  Both jobs run inside ONE cmake -P invocation: chaining
  # two quoted cmake.exe calls inside a single `cmd.exe /C "..."` makes cmd
  # strip only the outermost quotes and try to run the second path as a
  # command, which fails the link.
  #
  # Lists are joined with '?' rather than ';' (the shell would split it) and
  # rather than '|': ninja can nest this POST_BUILD inside another
  # `cmd.exe /C "..."`, and a '|' inside the nested quoted string makes cmd
  # mis-pair the quotes and run everything after it as a new command
  # ("'L:' is not recognized as an internal or external command").  '?' is
  # illegal in a Windows path and means nothing to cmd outside globbing.
  #
  # The delay-load list itself is filled in later by osv_add_delayload(); the
  # command reads it from the target property OSV_DELAYLOAD_DLLS through a
  # generator expression, so the order of the two calls does not matter.
  string(REPLACE ";" "?" _search_dirs_joined "${OSV_PLUGIN_DLL_SEARCH_DIRS}")
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMODULE=$<TARGET_FILE:${TARGET}>"
            "-DOUT_DIR=${_out_dir}"
            "-DSEARCH_DIRS=${_search_dirs_joined}"
            "-DEXTRA_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:${TARGET}>,?>"
            "-DDUMPBIN=${OSV_DUMPBIN_EXE}"
            "-DCHECK_DELAYLOAD=${OSV_CHECK_DELAYLOAD_SCRIPT}"
            "-DEXPECTED_DELAYLOAD=$<JOIN:$<TARGET_PROPERTY:${TARGET},OSV_DELAYLOAD_DLLS>,?>"
            -P "${OSV_COPY_RUNTIME_DLLS_SCRIPT}"
    COMMENT "Staging runtime DLLs for ${TARGET}${_suffix}"
    VERBATIM)
endfunction()

# =============================================================================
#  osv_add_pipl(<target> R_FILE <file.r> OUT_VAR <var>
#               [RC_FILE <file.rc>] [DEPENDS <file>...])
#
#  Implements the After Effects three-step PiPL pipeline for KIND aex
#  plug-ins (the Premiere SDK's Cnvtpipl.exe cannot parse AE-kind PiPLs):
#
#    1. cl /I <AE Headers> /I <AE Resources> /I <Premiere Headers>
#          /I <dir of file.r> /D PRWIN_ENV /D MSWindows /EP file.r > file.rr
#    2. PiPLtool.exe file.rr file.rrc
#    3. cl /D MSWindows /EP file.rrc > file.rcp
#
#  All three run through cmake/OsvPiplStep.cmake so no shell redirection is
#  needed.  Step 1 also records every header the .r pulled in (cl
#  /showIncludes) into a depfile, so editing AE_General.r, AE_EffectVers.h or
#  a project header such as ReframeParams.h regenerates the PiPL.  Extra
#  DEPENDS entries are honoured as well.
#
#  OUT_VAR receives the absolute path of the generated .rcp.  The directory
#  that holds it is added to the target's include path (rc.exe honours
#  target include directories) so the plug-in's .rc can simply
#  `#include "<name>.rcp"`.  When RC_FILE is given, the .rc source is marked
#  as depending on the .rcp so it is recompiled after regeneration.
#
#  INCLUDES adds extra directories to step 1's search path, AFTER the .r's own
#  directory and before the SDKs.  It exists because step 1 is a standalone
#  cl /EP invocation and does NOT read the target's include directories: a .r
#  that includes a header from a shared directory (as the source settings
#  effect's does, for the match name in plugins/common) has no other way to
#  find it, and copying the header next to every .r that needs it is exactly
#  the duplication these single-source-of-truth headers exist to prevent.
# =============================================================================
function(osv_add_pipl TARGET)
  set(_options)
  set(_one R_FILE OUT_VAR RC_FILE)
  set(_multi DEPENDS INCLUDES)
  cmake_parse_arguments(ARG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(NOT ARG_R_FILE)
    message(FATAL_ERROR "osv_add_pipl(${TARGET}): R_FILE is required")
  endif()
  if(NOT ARG_OUT_VAR)
    message(FATAL_ERROR "osv_add_pipl(${TARGET}): OUT_VAR is required")
  endif()
  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "osv_add_pipl(${TARGET}): '${TARGET}' is not a target; call osv_add_premiere_plugin first")
  endif()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "osv_add_pipl(${TARGET}): unknown arguments '${ARG_UNPARSED_ARGUMENTS}'")
  endif()

  get_filename_component(_r_abs  "${ARG_R_FILE}" ABSOLUTE)
  get_filename_component(_r_name "${_r_abs}" NAME_WE)
  get_filename_component(_r_dir  "${_r_abs}" DIRECTORY)
  if(NOT EXISTS "${_r_abs}")
    message(FATAL_ERROR "osv_add_pipl(${TARGET}): PiPL source '${_r_abs}' does not exist")
  endif()

  # macOS: Rez, the way the After Effects Xcode samples' Rez phase runs it,
  # straight to a data-fork resource file the bundle carries in
  # Contents/Resources/<target>.rsrc.  The include order is the same as
  # step 1 below (ours first, then the SDKs), -d __MACH__ is what those
  # samples define for AEConfig.h, and -arch lets AEConfig.h recognise the
  # processor inside Rez.  RC_FILE has no meaning here and is ignored.
  if(APPLE)
    set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/pipl/${TARGET}")
    file(MAKE_DIRECTORY "${_gen_dir}")
    set(_rsrc "${_gen_dir}/${TARGET}.rsrc")
    set(_rez_includes "")
    foreach(_inc IN ITEMS "${_r_dir}" ${ARG_INCLUDES} "${OSV_AE_SDK_HEADERS}" "${OSV_AE_SDK_RESOURCES}"
                          "${OSV_PREMIERE_SDK_HEADERS}")
      list(APPEND _rez_includes -i "${_inc}")
    endforeach()
    set(_rez_sysroot "")
    if(OSV_MACOS_SDK_PATH)
      set(_rez_sysroot -isysroot "${OSV_MACOS_SDK_PATH}")
    endif()
    add_custom_command(
      OUTPUT  "${_rsrc}"
      COMMAND "${OSV_REZ_EXE}" -o "${_rsrc}" -d SystemSevenOrLater=1 -useDF -script Roman -d __MACH__
              -arch arm64 ${_rez_sysroot} ${_rez_includes} "${_r_abs}"
      DEPENDS "${_r_abs}" ${ARG_DEPENDS}
      COMMENT "Rez ${_r_name}.r -> ${TARGET}.rsrc"
      VERBATIM)
    target_sources(${TARGET} PRIVATE "${_rsrc}")
    set_source_files_properties("${_rsrc}" PROPERTIES GENERATED TRUE MACOSX_PACKAGE_LOCATION Resources)
    set(${ARG_OUT_VAR} "${_rsrc}" PARENT_SCOPE)
    return()
  endif()

  set(_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/pipl/${TARGET}")
  file(MAKE_DIRECTORY "${_gen_dir}")
  set(_rr      "${_gen_dir}/${_r_name}.rr")
  set(_rrc     "${_gen_dir}/${_r_name}.rrc")
  set(_rcp     "${_gen_dir}/${_r_name}.rcp")
  set(_depfile "${_gen_dir}/${_r_name}.rr.d")

  # Include search path for step 1: the plug-in's own directory first (so a
  # ReframeParams.h next to the .r is found), then any caller-supplied shared
  # directories, then the SDKs.  Ours come before Adobe's on purpose - a
  # project header must never be shadowed by a same-named SDK one.
  set(_includes "${_r_dir}" ${ARG_INCLUDES} "${OSV_AE_SDK_HEADERS}" "${OSV_AE_SDK_RESOURCES}"
                "${OSV_PREMIERE_SDK_HEADERS}")
  string(REPLACE ";" "|" _includes_joined "${_includes}")

  # Step 1: preprocess the .r (macros from AE_EffectVers.h and our own
  # headers expand here) and write the depfile.
  add_custom_command(
    OUTPUT  "${_rr}"
    COMMAND "${CMAKE_COMMAND}"
            -DSTEP=preprocess
            "-DCL=${OSV_MSVC_CL_EXE}"
            "-DINPUT=${_r_abs}"
            "-DOUTPUT=${_rr}"
            "-DDEPFILE=${_depfile}"
            "-DINCLUDES=${_includes_joined}"
            "-DDEFINES=PRWIN_ENV|MSWindows"
            -P "${OSV_PIPL_STEP_SCRIPT}"
    DEPENDS "${_r_abs}" "${OSV_PIPL_STEP_SCRIPT}" ${ARG_DEPENDS}
    DEPFILE "${_depfile}"
    COMMENT "PiPL step 1/3 (preprocess) ${_r_name}.r"
    VERBATIM)

  # Step 2: PiPLtool turns the Rez-style text into an .rc fragment.
  add_custom_command(
    OUTPUT  "${_rrc}"
    COMMAND "${CMAKE_COMMAND}"
            -DSTEP=pipltool
            "-DPIPLTOOL=${OSV_AE_PIPLTOOL}"
            "-DINPUT=${_rr}"
            "-DOUTPUT=${_rrc}"
            -P "${OSV_PIPL_STEP_SCRIPT}"
    DEPENDS "${_rr}" "${OSV_AE_PIPLTOOL}" "${OSV_PIPL_STEP_SCRIPT}"
    COMMENT "PiPL step 2/3 (PiPLtool) ${_r_name}.rr"
    VERBATIM)

  # Step 3: a second preprocessor pass resolves the MSWindows conditionals
  # PiPLtool leaves in its output.
  add_custom_command(
    OUTPUT  "${_rcp}"
    COMMAND "${CMAKE_COMMAND}"
            -DSTEP=preprocess
            "-DCL=${OSV_MSVC_CL_EXE}"
            "-DINPUT=${_rrc}"
            "-DOUTPUT=${_rcp}"
            "-DDEFINES=MSWindows"
            -P "${OSV_PIPL_STEP_SCRIPT}"
    DEPENDS "${_rrc}" "${OSV_PIPL_STEP_SCRIPT}"
    COMMENT "PiPL step 3/3 (finalise) ${_r_name}.rcp"
    VERBATIM)

  # Attach the generated file to the target so the chain is built, and make
  # it visible to rc.exe through the include path.
  target_sources(${TARGET} PRIVATE "${_rcp}")
  set_source_files_properties("${_rcp}" PROPERTIES HEADER_FILE_ONLY TRUE GENERATED TRUE)
  target_include_directories(${TARGET} PRIVATE "${_gen_dir}")
  if(ARG_RC_FILE)
    get_filename_component(_rc_abs "${ARG_RC_FILE}" ABSOLUTE)
    set_source_files_properties("${_rc_abs}" PROPERTIES OBJECT_DEPENDS "${_rcp}")
  endif()

  set(${ARG_OUT_VAR} "${_rcp}" PARENT_SCOPE)
endfunction()

# =============================================================================
#  osv_add_delayload(<target> DLL_NAMES <name.dll>...)
#
#  Marks the given DLLs as delay-loaded (/DELAYLOAD:<name>) and links
#  delayimp.lib.  Together with plugins/common/DelayLoad.cpp this makes the
#  plug-in resolve its FFmpeg / OpenCL DLLs from its own folder instead of
#  Premiere's application directory (which ships its own, older FFmpeg).
#  Names must match the DLL file names exactly, e.g. "avcodec-63.dll".
# =============================================================================
function(osv_add_delayload TARGET)
  set(_options)
  set(_one)
  set(_multi DLL_NAMES)
  cmake_parse_arguments(ARG "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(NOT TARGET ${TARGET})
    message(FATAL_ERROR "osv_add_delayload(${TARGET}): '${TARGET}' is not a target")
  endif()
  if(NOT ARG_DLL_NAMES)
    message(FATAL_ERROR "osv_add_delayload(${TARGET}): DLL_NAMES is required")
  endif()
  # macOS bundles solve the same problem with their own, uniquely named
  # copies of the libraries (OsvMacBundle.cmake); there is nothing to delay.
  if(APPLE)
    return()
  endif()
  if(NOT MSVC)
    message(WARNING "osv_add_delayload(${TARGET}): delay loading is only supported with MSVC")
    return()
  endif()

  foreach(_dll IN LISTS ARG_DLL_NAMES)
    target_link_options(${TARGET} PRIVATE "/DELAYLOAD:${_dll}")
  endforeach()
  target_link_libraries(${TARGET} PRIVATE delayimp)

  # Record the list on the target so the POST_BUILD audit installed by
  # osv_add_premiere_plugin() can check the linked module against it (the two
  # functions are called in either order, hence a property rather than a
  # variable).
  set_property(TARGET ${TARGET} APPEND PROPERTY OSV_DELAYLOAD_DLLS ${ARG_DLL_NAMES})
endfunction()
