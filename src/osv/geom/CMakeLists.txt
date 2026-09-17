# osv_geom: lens model, stream scaling, extrinsics, virtual cameras, equirect
# maps, attitude / stabilisation and blend weights (host reference math).
file(GLOB OSV_GEOM_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/geom/*.h")
file(GLOB OSV_GEOM_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_geom STATIC ${OSV_GEOM_SOURCES} ${OSV_GEOM_HEADERS})
add_library(osv::geom ALIAS osv_geom)

target_link_libraries(osv_geom
  PUBLIC osv_core osv_meta
)
target_include_directories(osv_geom PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
)
set_target_properties(osv_geom PROPERTIES FOLDER "osv")
