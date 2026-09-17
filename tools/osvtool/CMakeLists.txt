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

set_target_properties(osvtool PROPERTIES FOLDER "tools")

# Copy runtime DLLs (FFmpeg etc.) next to the executable after each build.
add_custom_command(TARGET osvtool POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:osvtool> $<TARGET_FILE_DIR:osvtool>
  COMMAND_EXPAND_LISTS)
