# =============================================================================
#  osvtool - command line front end for the OpenOSV library.
#  Sources are globbed so adding a command file needs no build-script change.
# =============================================================================
file(GLOB OSVTOOL_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_executable(osvtool ${OSVTOOL_SOURCES})

set(_osvtool_libs osv_core CLI11::CLI11 nlohmann_json::nlohmann_json)
foreach(_m container meta color video geom render io)
  if(TARGET osv_${_m})
    list(APPEND _osvtool_libs osv_${_m})
    string(TOUPPER "${_m}" _M)
    target_compile_definitions(osvtool PRIVATE OSV_HAVE_${_M}=1)
  endif()
endforeach()
target_link_libraries(osvtool PRIVATE ${_osvtool_libs})

# render --use-user-defaults reads the Source Settings defaults the Premiere
# plug-ins save, with the plug-ins' own (SDK-free) code, so the file means
# exactly the same thing to both.  Only PrefsBlob.h and UserDefaults.h are
# included from plugins/common; nothing there needs the Adobe SDKs.
target_sources(osvtool PRIVATE "${CMAKE_SOURCE_DIR}/plugins/common/UserDefaults.cpp")
target_include_directories(osvtool PRIVATE "${CMAKE_SOURCE_DIR}/plugins/common")

# -----------------------------------------------------------------------------
#  render --engine plugin (the default): the plug-ins' own clip engine
#
#  Exactly the sources the OpenFX bundle compiles for its "OpenOSV Source"
#  (plugins/ofx/CMakeLists.txt, "the importer's clip engine" and "the shared,
#  SDK-free plug-in layer"), with OSV_CLIP_ENGINE_WITHOUT_PREMIERE so no Adobe
#  header is ever included.  A render is then the frame Premiere and Resolve
#  show for the same Source Settings, not a second implementation of it.
# -----------------------------------------------------------------------------
set(_osvtool_importer "${CMAKE_SOURCE_DIR}/plugins/importer")
set(_osvtool_common   "${CMAKE_SOURCE_DIR}/plugins/common")
target_sources(osvtool PRIVATE
  "${_osvtool_importer}/ImporterInstance.cpp"
  "${_osvtool_importer}/SteadyStage.cpp"
  "${_osvtool_importer}/SceneLightStage.cpp"
  "${_osvtool_importer}/FlareStage.cpp"
  "${_osvtool_importer}/ProtectorGuard.cpp"
  "${_osvtool_importer}/ImporterAudio.cpp"
  "${_osvtool_importer}/ImporterGpuFrame.cpp"
  # No direct GPU path outside Premiere (the Open 360 Reframe effect's).
  "${_osvtool_importer}/EngineNoDirect.cpp"
  "${_osvtool_common}/HostContext.cpp"
  "${_osvtool_common}/PixelCopy.cpp")
if(WIN32)
  target_sources(osvtool PRIVATE "${_osvtool_common}/PluginLog.cpp")
else()
  target_sources(osvtool PRIVATE "${_osvtool_common}/PluginLogPosix.cpp")
endif()
target_include_directories(osvtool PRIVATE "${_osvtool_importer}")
target_compile_definitions(osvtool PRIVATE OSV_CLIP_ENGINE_WITHOUT_PREMIERE=1)
if(TARGET osv_ffmpeg)
  target_link_libraries(osvtool PRIVATE osv_ffmpeg)  # the clip engine decodes its own audio track
endif()
if(WIN32 AND OSV_ENABLE_CUDA)
  # The clip engine drives NVDEC through the CUDA DRIVER API.  nvcuda.dll
  # comes with NVIDIA's display driver, so it is DELAY-loaded: osvtool must
  # still start on a machine with an AMD or Intel GPU and no NVIDIA driver,
  # where the engine never reaches a driver call (it asks the runtime for a
  # device first, as the plug-ins do) and renders with OpenCL or the CPU.
  target_link_libraries(osvtool PRIVATE CUDA::cuda_driver delayimp)
  target_link_options(osvtool PRIVATE "/DELAYLOAD:nvcuda.dll")
endif()

set_target_properties(osvtool PROPERTIES FOLDER "tools")

# Copy runtime DLLs (FFmpeg etc.) next to the executable after each build.
# Windows only: elsewhere the list is always empty (shared libraries are found
# through the RPATH CMake writes into the build tree), and copy_if_different
# with nothing to copy is an error.
if(WIN32)
  add_custom_command(TARGET osvtool POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:osvtool> $<TARGET_FILE_DIR:osvtool>
    COMMAND_EXPAND_LISTS)
endif()
