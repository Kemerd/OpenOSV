# osv_color: D-Log M decode, BT.2100 transfers, matrices, .cube LUTs, auto-detect.
file(GLOB OSV_COLOR_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/color/*.h")
file(GLOB OSV_COLOR_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_color STATIC ${OSV_COLOR_SOURCES} ${OSV_COLOR_HEADERS})
add_library(osv::color ALIAS osv_color)

target_link_libraries(osv_color
  PUBLIC  osv_warnings osv_core osv_meta
)
target_include_directories(osv_color PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)
set_target_properties(osv_color PROPERTIES FOLDER "osv")
