# osv_meta: protobuf wire decoding, typed djmd metadata, format detection,
# calibration selection and JSON serialisation.
file(GLOB OSV_META_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/meta/*.h")
file(GLOB OSV_META_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_meta STATIC ${OSV_META_SOURCES} ${OSV_META_HEADERS})
add_library(osv::meta ALIAS osv_meta)

target_link_libraries(osv_meta
  PUBLIC  osv_core osv_container nlohmann_json::nlohmann_json
)
target_include_directories(osv_meta PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)
set_target_properties(osv_meta PROPERTIES FOLDER "osv")
