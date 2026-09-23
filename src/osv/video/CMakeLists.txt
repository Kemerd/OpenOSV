# osv_video: FFmpeg (LGPL, dynamic) HEVC / H.264 decoding, frame-accurate
# seeking, the dual-lens reader, raw elementary-stream extraction and the IMU
# CSV export.
file(GLOB OSV_VIDEO_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/video/*.h")
file(GLOB OSV_VIDEO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_video STATIC ${OSV_VIDEO_SOURCES} ${OSV_VIDEO_HEADERS})
add_library(osv::video ALIAS osv_video)

target_link_libraries(osv_video
  PUBLIC  osv_core osv_container osv_meta osv_ffmpeg
)
target_include_directories(osv_video PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)

# The CUDA hwaccel path (device-resident frames, device-count validation)
# only compiles when the CUDA runtime is part of the build; without it the
# decoder still offers the CUDA back-end through FFmpeg alone.
#
# GpuClipDecoder (the direct GPU pipeline's VRAM frame cache) uses the CUDA
# DRIVER API only - nvcuda.dll, installed with the display driver - because
# the Premiere modules that will host it must not depend on a cudart DLL.
# The static library only drags nvcuda.dll into a module that actually calls
# GpuClipDecoder; HevcStreamDecoder itself makes no driver calls.
if(OSV_ENABLE_CUDA)
  target_link_libraries(osv_video PUBLIC CUDA::cudart_static)
  target_link_libraries(osv_video PRIVATE CUDA::cuda_driver)
  target_compile_definitions(osv_video PUBLIC OSV_VIDEO_HAVE_CUDA=1)
endif()

set_target_properties(osv_video PROPERTIES FOLDER "osv")
