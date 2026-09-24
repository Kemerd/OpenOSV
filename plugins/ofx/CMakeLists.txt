# =============================================================================
#  OpenOSV.ofx - the OpenFX plug-ins, for DaVinci Resolve (docs/RESOLVE.md)
#
#  One module, two effects:
#    "OpenOSV Source"    a generator that stitches a .OSV clip with the
#                        importer's own clip engine, as the full sphere or a
#                        reframed view;
#    "OpenOSV 360 Reframe"  a filter that reframes any equirectangular clip,
#                        on the CPU or on the host's CUDA images.
#
#  Laid out as an OpenFX bundle, so it can be copied as it is into the
#  folder every OpenFX host scans:
#
#    Windows  C:\Program Files\Common Files\OFX\Plugins\
#               OpenOSV.ofx.bundle\Contents\Win64\OpenOSV.ofx   (+ its DLLs)
#    macOS    /Library/OFX/Plugins/
#               OpenOSV.ofx.bundle/Contents/MacOS/OpenOSV.ofx   (+ Frameworks/)
#
#  NO ADOBE SDK.  The module compiles the Premiere plug-ins' host-independent
#  sources unchanged - the importer's clip engine, the reframe effect's
#  camera, the Source Settings mapping and the shared log / host context /
#  pixel code - so a clip stitched and framed here is the clip Premiere shows,
#  but it never includes an Adobe header: the clip engine is compiled with
#  OSV_CLIP_ENGINE_WITHOUT_PREMIERE (ImporterInstance.cpp's one Premiere-only
#  log detail), and the shared sources are compiled here rather than taken
#  from osv_premiere_common, which carries the Premiere SDK.  So the bundle
#  builds in every configuration - CI and a Mac without Adobe's SDKs
#  included - and its tests (tests/ofx) run there too.
# =============================================================================

if(NOT WIN32 AND NOT APPLE)
  message(STATUS "OpenOSV.ofx: Windows and macOS only - skipped on ${CMAKE_SYSTEM_NAME}")
  return()
endif()

# Where the bundle is assembled; scripts/install_ofx.ps1 (Windows) and
# scripts/install_ofx.sh (macOS) copy it from here.
set(OSV_OFX_STAGE_DIR "${CMAKE_BINARY_DIR}/plugins/ofx"
    CACHE PATH "Directory where OpenOSV.ofx.bundle is assembled")
file(TO_CMAKE_PATH "${OSV_OFX_STAGE_DIR}" OSV_OFX_STAGE_DIR)
set(OSV_OFX_BUNDLE_DIR "${OSV_OFX_STAGE_DIR}/OpenOSV.ofx.bundle")
file(MAKE_DIRECTORY "${OSV_OFX_STAGE_DIR}")

set(_osv_ofx_common   "${CMAKE_SOURCE_DIR}/plugins/common")
set(_osv_ofx_importer "${CMAKE_SOURCE_DIR}/plugins/importer")
set(_osv_ofx_reframe  "${CMAKE_SOURCE_DIR}/plugins/reframe")
set(_osv_ofx_settings "${CMAKE_SOURCE_DIR}/plugins/sourcesettings")

# -----------------------------------------------------------------------------
#  1. The CUDA kernel of the Reframe filter (Windows with CUDA only; without
#     it the filter renders on the CPU and does not advertise CUDA render)
# -----------------------------------------------------------------------------
set(_osv_ofx_fatbin_cpp "")
set(_osv_ofx_kernel_report "none (CPU render only)")
if(WIN32 AND OSV_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
  include("${CMAKE_SOURCE_DIR}/cmake/OsvCudaFatbin.cmake")
  osv_add_cuda_fatbin(OfxReframeKernel
    SOURCE   "${CMAKE_CURRENT_SOURCE_DIR}/OfxReframeKernel.cu"
    SYMBOL   kOsvOfxReframeFatbin
    OUT_CPP  _osv_ofx_fatbin_cpp
    REPORT   _osv_ofx_kernel_report
    INCLUDES "${CMAKE_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}"
    DEPENDS  "${CMAKE_CURRENT_SOURCE_DIR}/OfxKernelAbi.h"
             "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"
             "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h")
endif()

# -----------------------------------------------------------------------------
#  2. The sources
# -----------------------------------------------------------------------------
set(_osv_ofx_sources
  # ---- OpenFX: exports, host glue, the two effects ----
  OfxMain.cpp
  OfxHost.cpp
  OfxHost.h
  OfxCamera.cpp
  OfxCamera.h
  OfxRender.cpp
  OfxRender.h
  OfxReframe.cpp
  OfxReframe.h
  OfxCuda.cpp
  OfxCuda.h
  OfxKernelAbi.h
  OfxSource.cpp
  OfxSource.h
  OfxSourceParams.cpp
  OfxSourceParams.h
  OfxFileDialog.cpp
  OfxFileDialog.h
  # ---- the camera, exactly the Premiere effect's ----
  "${_osv_ofx_reframe}/ReframeCpu.cpp"
  "${_osv_ofx_reframe}/ReframeEasing.cpp"
  # ---- the Source Settings controls -> PrefsBlob ----
  "${_osv_ofx_settings}/SourceSettingsMapping.cpp"
  # ---- the importer's clip engine ----
  "${_osv_ofx_importer}/ImporterInstance.cpp"
  "${_osv_ofx_importer}/SteadyStage.cpp"
  "${_osv_ofx_importer}/FlareStage.cpp"
  "${_osv_ofx_importer}/ProtectorGuard.cpp"
  "${_osv_ofx_importer}/ImporterAudio.cpp"
  "${_osv_ofx_importer}/ImporterGpuFrame.cpp"
  # The Engine.h answers for a build with no direct GPU path - which this
  # module is on every platform: Premiere's direct path serves Open 360
  # Reframe inside Premiere and has no counterpart in an OpenFX host.
  "${_osv_ofx_importer}/EngineNoDirect.cpp"
  # ---- the shared, SDK-free plug-in layer ----
  "${_osv_ofx_common}/HostContext.cpp"
  "${_osv_ofx_common}/PixelCopy.cpp"
  ${_osv_ofx_fatbin_cpp}
)
if(WIN32)
  list(APPEND _osv_ofx_sources
    "${_osv_ofx_common}/PluginLog.cpp"
    "${_osv_ofx_common}/DelayLoad.cpp"
    OpenOSV.rc)
else()
  list(APPEND _osv_ofx_sources
    "${_osv_ofx_common}/PluginLogPosix.cpp"
    # The Choose .OSV File panel (NSOpenPanel).
    OfxFileDialogMac.mm)
endif()

# -----------------------------------------------------------------------------
#  3. The module, in each platform's OpenFX bundle layout
# -----------------------------------------------------------------------------
if(APPLE)
  include(OsvMacBundle)
  # OpenOSV.ofx.bundle/Contents/MacOS/OpenOSV.ofx, its FFmpeg embedded in
  # Contents/Frameworks under OpenOSV-prefixed names, exporting the three
  # OpenFX entry points and nothing else (cmake/OsvMacBundle.cmake).
  osv_add_mac_bundle(OpenOSV
    EXTENSION bundle
    PACKAGE_TYPE BNDL
    SIGNATURE "????"
    OUTPUT_NAME "OpenOSV.ofx"
    OUTPUT_DIRECTORY "${OSV_OFX_STAGE_DIR}"
    EXPORTS OfxGetNumberOfPlugins OfxGetPlugin OfxSetHost
    SOURCES ${_osv_ofx_sources})
  set_source_files_properties(OfxFileDialogMac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  target_link_libraries(OpenOSV PRIVATE
    "-framework Foundation" "-framework AppKit" "-framework UniformTypeIdentifiers")
  set(_osv_ofx_binary_dir "${OSV_OFX_BUNDLE_DIR}/Contents/MacOS")
else()
  set(_osv_ofx_binary_dir "${OSV_OFX_BUNDLE_DIR}/Contents/Win64")
  file(MAKE_DIRECTORY "${_osv_ofx_binary_dir}")
  # vcpkg's toolchain wraps add_library() and, with VCPKG_APPLOCAL_DEPS on,
  # copies DLLs next to every module it builds - from the WRONG tree for a
  # plug-in (see OsvPremiereSdk.cmake).  The post-build step below does it
  # properly, so vcpkg's is off for this one target.
  set(_osv_ofx_saved_applocal "${VCPKG_APPLOCAL_DEPS}")
  set(VCPKG_APPLOCAL_DEPS OFF)
  add_library(OpenOSV MODULE ${_osv_ofx_sources})
  set(VCPKG_APPLOCAL_DEPS "${_osv_ofx_saved_applocal}")
  set_target_properties(OpenOSV PROPERTIES
    PREFIX ""
    SUFFIX ".ofx"
    RUNTIME_OUTPUT_DIRECTORY "$<1:${_osv_ofx_binary_dir}>"
    LIBRARY_OUTPUT_DIRECTORY "$<1:${_osv_ofx_binary_dir}>"
    PDB_OUTPUT_DIRECTORY     "$<1:${_osv_ofx_binary_dir}>"
    # /MD: a host loads many plug-ins into one process, and a static CRT per
    # module exhausts the process's fiber-local storage slots.
    MSVC_RUNTIME_LIBRARY     "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    POSITION_INDEPENDENT_CODE ON
    FOLDER "plugins"
  )
  target_link_options(OpenOSV PRIVATE /SUBSYSTEM:WINDOWS)
endif()

target_include_directories(OpenOSV PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/openfx/include"
  "${_osv_ofx_common}"
  "${_osv_ofx_reframe}"
  "${_osv_ofx_importer}"
  "${_osv_ofx_settings}"
)
target_compile_definitions(OpenOSV PRIVATE OSV_CLIP_ENGINE_WITHOUT_PREMIERE=1)

target_link_libraries(OpenOSV PRIVATE
  osv_warnings
  osv_render            # the whole library chain
  osv_ffmpeg            # the clip engine decodes its own audio track
)
if(WIN32)
  target_link_libraries(OpenOSV PRIVATE comdlg32)  # the Choose .OSV File dialog
endif()

if(_osv_ofx_fatbin_cpp)
  target_compile_definitions(OpenOSV PRIVATE OSV_OFX_HAVE_CUDA=1)
endif()
if(WIN32 AND OSV_ENABLE_CUDA)
  # The clip engine's GPU frame path drives NVDEC through the driver API; the
  # filter launches its kernel through it.
  target_link_libraries(OpenOSV PRIVATE CUDA::cuda_driver)
endif()
# The importer's 16u / 8u pack kernel, when a Premiere build made it.
if(TARGET osv_importer_pack)
  target_link_libraries(OpenOSV PRIVATE osv_importer_pack)
  target_compile_definitions(OpenOSV PRIVATE OSV_IMPORTER_HAVE_PACK_KERNEL=1)
endif()

# Where the bundle is, for the tests and the packaging scripts.
set_target_properties(OpenOSV PROPERTIES OSV_OFX_BUNDLE_DIR "${OSV_OFX_BUNDLE_DIR}")

# -----------------------------------------------------------------------------
#  4. Windows: runtime DLLs beside the module, and delay loading
#
#  The same two mechanisms, and the same two scripts, as the Premiere
#  plug-ins (cmake/OsvPremiereSdk.cmake):
#
#    * the post-build step copies every DLL the module imports - walked
#      recursively with dumpbin - from vcpkg's bin folder into Contents\Win64;
#    * every one of them is /DELAYLOAD'ed and resolved from that folder by
#      the hook in plugins/common/DelayLoad.cpp, so a host that ships its own
#      FFmpeg (or a machine with no OpenCL ICD, or no NVIDIA driver) never
#      binds the module to the wrong copy or refuses to load it; the step
#      then audits that nothing else is imported directly.
# -----------------------------------------------------------------------------
if(WIN32)
  get_filename_component(_osv_ofx_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
  find_program(OSV_DUMPBIN_EXE NAMES dumpbin HINTS "${_osv_ofx_cl_dir}"
               DOC "MSVC dumpbin.exe (runtime DLL discovery)")
  if(NOT OSV_DUMPBIN_EXE)
    message(WARNING "dumpbin.exe was not found; OpenOSV.ofx's post-build step can only copy DLLs CMake knows. "
                    "Configure from a Visual Studio developer shell.")
  endif()

  set(_osv_ofx_search_dirs "")
  if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
    list(APPEND _osv_ofx_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin")
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
      list(APPEND _osv_ofx_search_dirs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/debug/bin")
    endif()
  endif()
  list(APPEND _osv_ofx_search_dirs "${CMAKE_BINARY_DIR}/bin")
  string(REPLACE ";" "?" _osv_ofx_search_joined "${_osv_ofx_search_dirs}")

  # The delay-loaded set: exactly what the module imports from vcpkg, plus
  # the NVIDIA driver.  Names are read from the installed tree where CMake
  # does not know them (the FFmpeg import libraries are plain .lib paths).
  set(_osv_ofx_delayload fmt.dll spdlog.dll)
  if(OSV_ENABLE_CUDA)
    list(APPEND _osv_ofx_delayload nvcuda.dll)
  endif()
  if(OSV_ENABLE_OPENCL)
    list(APPEND _osv_ofx_delayload OpenCL.dll)
  endif()
  foreach(_ff IN ITEMS avcodec avformat avutil swresample swscale)
    file(GLOB _ff_dlls "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin/${_ff}-*.dll")
    foreach(_ff_dll IN LISTS _ff_dlls)
      get_filename_component(_ff_name "${_ff_dll}" NAME)
      list(APPEND _osv_ofx_delayload "${_ff_name}")
    endforeach()
  endforeach()
  list(REMOVE_DUPLICATES _osv_ofx_delayload)
  foreach(_dll IN LISTS _osv_ofx_delayload)
    target_link_options(OpenOSV PRIVATE "/DELAYLOAD:${_dll}")
  endforeach()
  target_link_libraries(OpenOSV PRIVATE delayimp)
  # OsvCheckDelayLoad.cmake reads the list '?'-separated.
  string(REPLACE ";" "?" _osv_ofx_delayload_joined "${_osv_ofx_delayload}")

  add_custom_command(TARGET OpenOSV POST_BUILD
    COMMAND "${CMAKE_COMMAND}"
            "-DMODULE=$<TARGET_FILE:OpenOSV>"
            "-DOUT_DIR=${_osv_ofx_binary_dir}"
            "-DSEARCH_DIRS=${_osv_ofx_search_joined}"
            "-DEXTRA_DLLS=$<JOIN:$<TARGET_RUNTIME_DLLS:OpenOSV>,?>"
            "-DDUMPBIN=${OSV_DUMPBIN_EXE}"
            "-DCHECK_DELAYLOAD=${CMAKE_SOURCE_DIR}/cmake/OsvCheckDelayLoad.cmake"
            "-DEXPECTED_DELAYLOAD=${_osv_ofx_delayload_joined}"
            -P "${CMAKE_SOURCE_DIR}/cmake/OsvCopyRuntimeDlls.cmake"
    COMMENT "Staging the runtime DLLs of OpenOSV.ofx"
    VERBATIM)
else()
  set(_osv_ofx_delayload "none (the bundle's own renamed dylibs)")
endif()

# -----------------------------------------------------------------------------
#  5. The rest of the bundle: the licence of the vendored OpenFX headers
#     travels with the binary built from them.
# -----------------------------------------------------------------------------
add_custom_command(TARGET OpenOSV POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${OSV_OFX_BUNDLE_DIR}/Contents/Resources"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
          "${CMAKE_CURRENT_SOURCE_DIR}/openfx/LICENSE.md"
          "${OSV_OFX_BUNDLE_DIR}/Contents/Resources/OpenFX-LICENSE.md"
  COMMENT "Assembling OpenOSV.ofx.bundle"
  VERBATIM)

message(STATUS "OpenOSV.ofx")
message(STATUS "  Bundle        : ${OSV_OFX_BUNDLE_DIR}")
message(STATUS "  Kernel archs  : ${_osv_ofx_kernel_report}")
message(STATUS "  Delay-loaded  : ${_osv_ofx_delayload}")
