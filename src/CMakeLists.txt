# =============================================================================
#  Library modules.  Each module directory owns its own CMakeLists.txt so that
#  work on one module never touches another's build file.  The order below is
#  the dependency order (core first).
# =============================================================================
set(OSV_MODULES core container meta color video geom render io)

foreach(_module IN LISTS OSV_MODULES)
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/osv/${_module}/CMakeLists.txt")
    add_subdirectory("osv/${_module}")
  else()
    message(STATUS "Module '${_module}' has no CMakeLists.txt yet - skipped")
  endif()
endforeach()
