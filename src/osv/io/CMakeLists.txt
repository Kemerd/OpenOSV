# osv_io: still-image writers (libavcodec png/tiff, tinyexr) and the ffmpeg.exe pipe.
file(GLOB OSV_IO_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/io/*.h")
file(GLOB OSV_IO_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_io STATIC ${OSV_IO_SOURCES} ${OSV_IO_HEADERS})
add_library(osv::io ALIAS osv_io)

target_link_libraries(osv_io
  PUBLIC  osv_core osv_render osv_ffmpeg
  PRIVATE unofficial::tinyexr::tinyexr
)
set_target_properties(osv_io PROPERTIES FOLDER "osv")
