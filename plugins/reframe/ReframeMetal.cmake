# =============================================================================
#  ReframeMetal.cmake  (macOS only)
#
#  osv_reframe_metal - the effect's Metal GPU path as a static library:
#  ReframeMetal.mm plus its kernel library, assembled exactly like the
#  library's own Metal renderer (cmake/OsvMetal.cmake):
#
#      preamble.metal -> ColorMath.h -> osv_kernel.h -> ReframeKernel.metal
#
#  embedded as source and, when the build machine has the Metal compiler, as
#  a precompiled metallib as well.
#
#  It needs no Adobe SDK, which is why it is its own file: the Premiere
#  effect links it (plugins/reframe/CMakeLists.txt), and so does the macOS
#  test-suite, which drives the very same code with Metal objects of its own
#  on machines without the SDK.  PluginLog is NOT compiled in here - the
#  consumer provides it (osv_premiere_common in the effect, the test's own
#  copy of PluginLogPosix.cpp in the tests).
# =============================================================================
if(NOT APPLE OR TARGET osv_reframe_metal)
  return()
endif()
if(NOT TARGET osv_metal)
  message(FATAL_ERROR "the effect's Metal path needs the Metal backend (OSV_ENABLE_METAL=ON, cmake/OsvMetal.cmake)")
endif()

set(_osv_reframe_metal_dir "${CMAKE_SOURCE_DIR}/plugins/reframe")
set(_osv_reframe_metal_gen "${CMAKE_BINARY_DIR}/generated/plugins/reframe/metal")
osv_metal_library("${_osv_reframe_metal_gen}" ReframeKernel
  SOURCES "${CMAKE_SOURCE_DIR}/src/osv/render/metal/preamble.metal"
          "${CMAKE_SOURCE_DIR}/include/osv/color/ColorMath.h"
          "${CMAKE_SOURCE_DIR}/include/osv/render/osv_kernel.h"
          "${_osv_reframe_metal_dir}/ReframeKernel.metal"
  OUT_SOURCE _osv_reframe_metal_source
  OUT_METALLIB _osv_reframe_metal_lib)

osv_embed_text_file("${_osv_reframe_metal_gen}/ReframeMetalSource.cpp" "${_osv_reframe_metal_source}"
                    osv_reframe_metal_source)
set(_osv_reframe_metal_embedded "${_osv_reframe_metal_gen}/ReframeMetalSource.cpp")
if(_osv_reframe_metal_lib)
  osv_embed_text_file("${_osv_reframe_metal_gen}/ReframeMetalLib.cpp" "${_osv_reframe_metal_lib}"
                      osv_reframe_metal_lib)
  list(APPEND _osv_reframe_metal_embedded "${_osv_reframe_metal_gen}/ReframeMetalLib.cpp")
endif()

add_library(osv_reframe_metal STATIC
  "${_osv_reframe_metal_dir}/ReframeMetal.h"
  "${_osv_reframe_metal_dir}/ReframeMetal.mm"
  ${_osv_reframe_metal_embedded})
target_include_directories(osv_reframe_metal PUBLIC "${_osv_reframe_metal_dir}" "${CMAKE_SOURCE_DIR}/plugins/common")
target_link_libraries(osv_reframe_metal PUBLIC osv_warnings osv_core osv_metal)
if(_osv_reframe_metal_lib)
  target_compile_definitions(osv_reframe_metal PRIVATE OSV_REFRAME_HAVE_METALLIB=1)
endif()
# ARC for the Objective-C++ half (see ReframeMetal.mm).
set_source_files_properties("${_osv_reframe_metal_dir}/ReframeMetal.mm" PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
set_target_properties(osv_reframe_metal PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_VISIBILITY_PRESET hidden
  OBJCXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  FOLDER "plugins")
