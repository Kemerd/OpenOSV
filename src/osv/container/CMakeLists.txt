# osv_container: ISO BMFF box walking, sample tables, hvcC/colr, DJI index
# table and the trailing 'camd' movie.  Every span aliases the file mapping.
file(GLOB OSV_CONTAINER_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/container/*.h")
file(GLOB OSV_CONTAINER_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_container STATIC ${OSV_CONTAINER_SOURCES} ${OSV_CONTAINER_HEADERS})
add_library(osv::container ALIAS osv_container)

target_link_libraries(osv_container
  PUBLIC osv_core
)
target_include_directories(osv_container PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
)
set_target_properties(osv_container PROPERTIES FOLDER "osv")
