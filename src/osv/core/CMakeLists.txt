# osv_core: error handling, byte access, file mapping, math, threads, logging.
file(GLOB OSV_CORE_HEADERS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/include/osv/core/*.h")
file(GLOB OSV_CORE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")

add_library(osv_core STATIC ${OSV_CORE_SOURCES} ${OSV_CORE_HEADERS})
add_library(osv::core ALIAS osv_core)

target_link_libraries(osv_core
  PUBLIC  osv_warnings
  PRIVATE spdlog::spdlog fmt::fmt
)
target_include_directories(osv_core PUBLIC
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)
set_target_properties(osv_core PROPERTIES FOLDER "osv")
