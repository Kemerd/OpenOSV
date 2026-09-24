# =============================================================================
#  OsvMacBundle.cmake  (macOS only)
#
#  osv_add_mac_bundle(<target> EXTENSION bundle|plugin
#                     PACKAGE_TYPE <4cc> SIGNATURE <4cc>
#                     OUTPUT_DIRECTORY <dir> SOURCES <src>...
#                     EXPORTS <c-symbol>...
#                     [OUTPUT_NAME <name>])
#
#  A loadable bundle laid out the way Adobe's Xcode samples lay out a
#  Premiere Pro / After Effects plug-in, and made self-contained:
#
#    <dir>/<target>.<extension>/
#        Contents/Info.plist               from cmake/macos/PluginInfo.plist.in
#        Contents/MacOS/<target>           the module (MH_BUNDLE)
#        Contents/Resources/               whatever the caller marks with
#                                          MACOSX_PACKAGE_LOCATION Resources
#        Contents/Frameworks/OpenOSV_*.dylib   the shared libraries it loads
#
#  * EXPORTS names the entry points (xImportEntry, EffectMain, ...) and they
#    are the ONLY symbols the bundle exports (-exported_symbols_list), on top
#    of -fvisibility=hidden for its own sources.  The statically linked
#    library, fmt and spdlog stay private: a host loads many plug-ins into
#    one process, and dyld would otherwise coalesce their weak definitions
#    (inline functions, template statics) across bundles.  Adobe's samples
#    set "symbols private extern" for the same reason.
#  * The RPATH is @loader_path/../Frameworks from the first link on, so the
#    module never looks in the build tree or vcpkg_installed.
#  * The post-build step (OsvMacBundleDylibs.cmake) copies every @rpath dylib
#    the module loads into Contents/Frameworks under an "OpenOSV_" prefixed
#    install name and signs the result ad hoc - see that script for why the
#    prefix matters inside a host process.
#
#  * OUTPUT_NAME, when given, replaces <target> in the bundle's folder and
#    executable names (and in Info.plist): the OpenFX bundle is
#    OUTPUT_NAME "OpenOSV.ofx" with EXTENSION bundle, which is exactly the
#    OpenOSV.ofx.bundle/Contents/MacOS/OpenOSV.ofx the OpenFX packaging
#    rules ask for.
#
#  Used by the Premiere plug-ins (cmake/OsvPremiereSdk.cmake), by the OpenFX
#  plug-ins (plugins/ofx) and by the macOS test-suite, which builds an SDK-free probe bundle the same way and
#  loads it, so this machinery is exercised on machines without the Adobe
#  SDKs too.
# =============================================================================
include_guard(GLOBAL)

if(NOT APPLE)
  return()
endif()

# The post-build script and the Info.plist template are located inside
# osv_add_mac_bundle() itself, from CMAKE_CURRENT_FUNCTION_LIST_DIR - NOT set
# here as plain variables.  include_guard(GLOBAL) makes every include after
# the first a no-op, and a plain variable set by the first include lives only
# in THAT directory's scope: tests/macos includes this file before
# plugins/ofx does, so plugins/ofx saw both paths empty and configure_file()
# was handed its own source folder.

# Where the post-build step looks for the dylibs a bundle loads: vcpkg's lib
# folder for the triplet (Debug first in a Debug build).
if(NOT DEFINED OSV_MAC_BUNDLE_SEARCH_DIRS)
  set(_osv_mac_search "")
  if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      list(APPEND _osv_mac_search "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/lib")
    endif()
    list(APPEND _osv_mac_search "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
  endif()
  set(OSV_MAC_BUNDLE_SEARCH_DIRS "${_osv_mac_search}" CACHE STRING
      "Directories the macOS bundle post-build step copies shared libraries from")
endif()

function(osv_add_mac_bundle TARGET)
  cmake_parse_arguments(ARG "" "EXTENSION;PACKAGE_TYPE;SIGNATURE;OUTPUT_DIRECTORY;OUTPUT_NAME" "SOURCES;EXPORTS" ${ARGN})
  foreach(_required EXTENSION PACKAGE_TYPE SIGNATURE OUTPUT_DIRECTORY SOURCES EXPORTS)
    if(NOT ARG_${_required})
      message(FATAL_ERROR "osv_add_mac_bundle(${TARGET}): ${_required} is required")
    endif()
  endforeach()
  if(ARG_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "osv_add_mac_bundle(${TARGET}): unknown arguments '${ARG_UNPARSED_ARGUMENTS}'")
  endif()
  file(MAKE_DIRECTORY "${ARG_OUTPUT_DIRECTORY}")

  # This file's own folder, whichever directory scope calls the function (see
  # the note above the search directories).
  set(_dylibs_script "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/OsvMacBundleDylibs.cmake")
  set(_plist_template "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/macos/PluginInfo.plist.in")
  foreach(_helper IN ITEMS "${_dylibs_script}" "${_plist_template}")
    if(NOT EXISTS "${_helper}")
      message(FATAL_ERROR "osv_add_mac_bundle(${TARGET}): '${_helper}' is missing")
    endif()
  endforeach()

  add_library(${TARGET} MODULE ${ARG_SOURCES})

  # The name the bundle folder and its executable carry: the target's own
  # unless the caller needs another (see OUTPUT_NAME above).
  set(_name "${TARGET}")
  if(ARG_OUTPUT_NAME)
    set(_name "${ARG_OUTPUT_NAME}")
    set_target_properties(${TARGET} PROPERTIES OUTPUT_NAME "${_name}")
  endif()

  # The Info.plist, fully resolved here so nothing is left to the generator.
  set(OSV_BUNDLE_EXECUTABLE "${_name}")
  set(OSV_BUNDLE_IDENTIFIER "com.openosv.${TARGET}")
  set(OSV_BUNDLE_NAME "${_name}")
  set(OSV_BUNDLE_VERSION "${PROJECT_VERSION}")
  set(OSV_BUNDLE_PACKAGE_TYPE "${ARG_PACKAGE_TYPE}")
  set(OSV_BUNDLE_SIGNATURE "${ARG_SIGNATURE}")
  set(_plist "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}-Info.plist")
  configure_file("${_plist_template}" "${_plist}" @ONLY)

  set_target_properties(${TARGET} PROPERTIES
    BUNDLE TRUE
    BUNDLE_EXTENSION "${ARG_EXTENSION}"
    MACOSX_BUNDLE_INFO_PLIST "${_plist}"
    PREFIX ""
    LIBRARY_OUTPUT_DIRECTORY "$<1:${ARG_OUTPUT_DIRECTORY}>"
    CXX_VISIBILITY_PRESET hidden
    OBJCXX_VISIBILITY_PRESET hidden
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    BUILD_WITH_INSTALL_RPATH ON
    INSTALL_RPATH "@loader_path/../Frameworks"
    POSITION_INDEPENDENT_CODE ON
  )
  # Unused code and unused library references out of the module.
  target_link_options(${TARGET} PRIVATE "LINKER:-dead_strip" "LINKER:-dead_strip_dylibs")

  # The entry points, and nothing else, exported.  C symbols carry a leading
  # underscore in Mach-O.
  set(_exports_file "${CMAKE_CURRENT_BINARY_DIR}/${TARGET}.exports")
  set(_exports_text "")
  foreach(_symbol IN LISTS ARG_EXPORTS)
    string(APPEND _exports_text "_${_symbol}\n")
  endforeach()
  file(WRITE "${_exports_file}" "${_exports_text}")
  target_link_options(${TARGET} PRIVATE "LINKER:-exported_symbols_list,${_exports_file}")
  set_property(TARGET ${TARGET} APPEND PROPERTY LINK_DEPENDS "${_exports_file}")

  string(REPLACE ";" "?" _search_dirs_joined "${OSV_MAC_BUNDLE_SEARCH_DIRS}")
  add_custom_command(TARGET ${TARGET} POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DBUNDLE=$<TARGET_BUNDLE_DIR:${TARGET}>"
            "-DBINARY=$<TARGET_FILE:${TARGET}>"
            "-DSEARCH_DIRS=${_search_dirs_joined}"
            "-DPREFIX=OpenOSV_"
            -P "${_dylibs_script}"
    COMMENT "Embedding the shared libraries of ${TARGET}.${ARG_EXTENSION} and signing it"
    VERBATIM)
endfunction()
