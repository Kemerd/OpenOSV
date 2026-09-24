# =============================================================================
#  OpenOSV.ofx - the OpenFX plug-ins, for DaVinci Resolve (docs/RESOLVE.md)
#
#  One module, two effects:
#    "OpenOSV Source"    a generator that stitches a .OSV clip with the
#                        importer's own engine, as the full sphere or a
#                        reframed view;
#    "Open 360 Reframe"  a filter that reframes any equirectangular clip,
#                        on the CPU or on the host's CUDA images.
#
#  Laid out as an OpenFX bundle so it can be copied as-is into
#  C:\Program Files\Common Files\OFX\Plugins:
#
#    OpenOSV.ofx.bundle\Contents\Win64\OpenOSV.ofx     (+ its runtime DLLs)
#
#  The module is built with osv_add_premiere_plugin(KIND ofx) - the same /MD,
#  runtime-DLL staging and delay-load audit as the Premiere plug-ins - and it
#  compiles the Premiere plug-ins' host-independent sources unchanged:
#
#    plugins/importer      the clip engine (ImporterInstance and its stages)
#    plugins/reframe       the camera: parameters, lens maths, easing
#    plugins/sourcesettings the controls -> PrefsBlob mapping
#
#  so a clip stitched and framed here is the clip Premiere shows.  Those
#  sources include one Premiere SDK header (for a log line's colour-space
#  name), which is why this directory lives under OSV_BUILD_PREMIERE; no Adobe
#  code ends up in the module.
# =============================================================================

# Where the bundle is assembled; scripts/install_ofx.ps1 copies it from here.
set(OSV_OFX_STAGE_DIR "${CMAKE_BINARY_DIR}/plugins/ofx"
    CACHE PATH "Directory where OpenOSV.ofx.bundle is assembled")
file(TO_CMAKE_PATH "${OSV_OFX_STAGE_DIR}" OSV_OFX_STAGE_DIR)
set(OSV_OFX_BUNDLE_DIR "${OSV_OFX_STAGE_DIR}/OpenOSV.ofx.bundle")
set(OSV_OFX_BINARY_DIR "${OSV_OFX_BUNDLE_DIR}/Contents/Win64")
file(MAKE_DIRECTORY "${OSV_OFX_BINARY_DIR}")

set(_osv_ofx_importer "${CMAKE_SOURCE_DIR}/plugins/importer")
set(_osv_ofx_reframe  "${CMAKE_SOURCE_DIR}/plugins/reframe")
set(_osv_ofx_settings "${CMAKE_SOURCE_DIR}/plugins/sourcesettings")

# -----------------------------------------------------------------------------
#  1. The CUDA kernel of the Reframe filter (optional: without CUDA the filter
#     renders on the CPU only and does not advertise CUDA render)
# -----------------------------------------------------------------------------
set(_osv_ofx_fatbin_cpp "")
set(_osv_ofx_kernel_report "none (CPU render only)")
if(OSV_ENABLE_CUDA AND CMAKE_CUDA_COMPILER)
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
#  2. The module
# -----------------------------------------------------------------------------
osv_add_premiere_plugin(OpenOSV
  KIND ofx
  OUTPUT_DIRECTORY "${OSV_OFX_BINARY_DIR}"
  SOURCES
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
    # The two Source Settings publication hooks ImporterInstance links
    # against; Premiere's direct-path engine (Engine.cpp) has no consumer here.
    OfxEngineHooks.cpp
    OpenOSV.rc
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
    "${_osv_ofx_importer}/PrefsMapping.cpp"
    ${_osv_ofx_fatbin_cpp}
)

target_include_directories(OpenOSV PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/openfx/include"
  "${_osv_ofx_reframe}"
  "${_osv_ofx_importer}"
  "${_osv_ofx_settings}"
)

target_link_libraries(OpenOSV PRIVATE
  osv_premiere_common   # log, delay-load hook, host context, prefs, pixel copy
  osv_render            # the whole library chain
  osv_ffmpeg            # the clip engine decodes its own audio track
  comdlg32              # the Choose .OSV File dialog
)

if(_osv_ofx_fatbin_cpp)
  target_compile_definitions(OpenOSV PRIVATE OSV_OFX_HAVE_CUDA=1)
endif()
if(OSV_ENABLE_CUDA)
  # The clip engine's direct path and GPU frame path drive NVDEC and upload
  # through the driver API; the filter launches its kernel through it.
  target_link_libraries(OpenOSV PRIVATE CUDA::cuda_driver)
endif()
# The importer's 16u / 8u pack kernel, when the importer built it.
if(TARGET osv_importer_pack)
  target_link_libraries(OpenOSV PRIVATE osv_importer_pack)
  target_compile_definitions(OpenOSV PRIVATE OSV_IMPORTER_HAVE_PACK_KERNEL=1)
endif()

# The bundle's binary name is fixed by the OpenFX packaging rules
# (<name>.ofx.bundle/Contents/Win64/<name>.ofx), and the tests load it by path.
set_target_properties(OpenOSV PROPERTIES OSV_OFX_BUNDLE_DIR "${OSV_OFX_BUNDLE_DIR}")

# -----------------------------------------------------------------------------
#  3. Delay loading
#
#  The same set as the importer, for the same reason: DaVinci Resolve's own
#  directory may hold other copies of FFmpeg and friends, and the hook in
#  plugins/common/DelayLoad.cpp resolves ours from the bundle's Win64 folder
#  first.  nvcuda.dll comes from the system; delay-loading it keeps a machine
#  without an NVIDIA driver able to load the module at all.
# -----------------------------------------------------------------------------
set(_osv_ofx_delayload fmt.dll spdlog.dll)
if(OSV_ENABLE_CUDA)
  list(APPEND _osv_ofx_delayload nvcuda.dll)
endif()
if(OSV_ENABLE_OPENCL)
  list(APPEND _osv_ofx_delayload OpenCL.dll)
endif()
foreach(_ff IN ITEMS avcodec avformat avutil swresample swscale)
  file(GLOB _ff_dlls "${CMAKE_SOURCE_DIR}/vcpkg_installed/${VCPKG_TARGET_TRIPLET}/bin/${_ff}-*.dll")
  foreach(_ff_dll IN LISTS _ff_dlls)
    get_filename_component(_ff_name "${_ff_dll}" NAME)
    list(APPEND _osv_ofx_delayload "${_ff_name}")
  endforeach()
endforeach()
list(REMOVE_DUPLICATES _osv_ofx_delayload)
osv_add_delayload(OpenOSV DLL_NAMES ${_osv_ofx_delayload})

# -----------------------------------------------------------------------------
#  4. The rest of the bundle: the licence of the vendored OpenFX headers
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
