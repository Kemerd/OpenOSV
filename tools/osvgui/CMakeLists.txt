# =============================================================================
#  osvgui - OpenOSV Studio, the desktop app in front of osvtool: drop clips
#  and folders, pick the options, batch-render the queue (README, "OpenOSV
#  Studio").
#
#  Two targets:
#    osvgui_core  the UI-free half: the queue and its folder scan, the
#                 settings file, settings -> `osvtool render` arguments, and
#                 osvtool's output parsing.  Plain C++20 and nlohmann/json,
#                 built with the tools on every platform so the [gui] unit
#                 tests (tests/unit/test_gui.cpp) run everywhere, CI included.
#    osvgui       the app: Dear ImGui on GLFW + OpenGL 3.  Built when
#                 OSV_BUILD_GUI is on - the default when vcpkg provides the
#                 imgui and glfw3 ports (top-level CMakeLists.txt).
# =============================================================================

add_library(osvgui_core STATIC
  Queue.cpp
  Settings.cpp
  CommandBuilder.cpp
  Progress.cpp)
target_include_directories(osvgui_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(osvgui_core PRIVATE nlohmann_json::nlohmann_json osv_warnings)
target_compile_features(osvgui_core PUBLIC cxx_std_20)
set_target_properties(osvgui_core PROPERTIES FOLDER "tools")

if(NOT OSV_BUILD_GUI)
  return()
endif()

# -----------------------------------------------------------------------------
#  The app
# -----------------------------------------------------------------------------
add_executable(osvgui WIN32
  main.cpp
  Ui.cpp
  Widgets.cpp
  Style.cpp
  Runner.cpp
  Toolchain.cpp
  Platform.cpp
  Icon.cpp)
target_link_libraries(osvgui PRIVATE osvgui_core imgui::imgui glfw OpenGL::GL osv_warnings)
target_compile_features(osvgui PRIVATE cxx_std_20)
set_target_properties(osvgui PROPERTIES FOLDER "tools")

if(WIN32)
  # COM dialogs and the taskbar (ole32, uuid), the shell (shell32), the
  # themed title bar (dwmapi).
  target_link_libraries(osvgui PRIVATE ole32 uuid shell32 dwmapi)

  # ---- the icon: drawn by a build-time helper, no image in the repository ------
  add_executable(osvgui_icongen IconGen.cpp Icon.cpp)
  target_link_libraries(osvgui_icongen PRIVATE osv_warnings)
  set_target_properties(osvgui_icongen PROPERTIES
    FOLDER "tools"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  set(_osvgui_ico "${CMAKE_CURRENT_BINARY_DIR}/osvgui.ico")
  add_custom_command(
    OUTPUT "${_osvgui_ico}"
    COMMAND osvgui_icongen "${_osvgui_ico}"
    DEPENDS osvgui_icongen
    COMMENT "Drawing the OpenOSV Studio icon"
    VERBATIM)

  # ---- VERSIONINFO and the icon, as GLFW_ICON so GLFW uses it for the window ----
  set(OSVGUI_ICON_FILE "${_osvgui_ico}")
  configure_file(osvgui.rc.in "${CMAKE_CURRENT_BINARY_DIR}/osvgui.rc" @ONLY)
  target_sources(osvgui PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/osvgui.rc" "${_osvgui_ico}")
  set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/osvgui.rc" PROPERTIES OBJECT_DEPENDS "${_osvgui_ico}")

  # glfw3.dll (and anything else it imports from vcpkg) beside the exe.
  add_custom_command(TARGET osvgui POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:osvgui> $<TARGET_FILE_DIR:osvgui>
    COMMAND_EXPAND_LISTS)
elseif(APPLE)
  # AppKit open panels, NSWorkspace, the power assertion (Platform_mac.mm).
  target_sources(osvgui PRIVATE Platform_mac.mm)
  set_source_files_properties(Platform_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
  set_target_properties(osvgui PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED ON)
  target_compile_definitions(osvgui PRIVATE GL_SILENCE_DEPRECATION=1)
  target_link_libraries(osvgui PRIVATE
    "-framework Foundation" "-framework AppKit" "-framework UniformTypeIdentifiers"
    "-framework IOKit" "-framework CoreFoundation" "-framework Cocoa" "-framework CoreVideo"
    "-framework QuartzCore")
endif()
