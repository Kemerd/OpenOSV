# =============================================================================
#  OsvPremiereSdk.cmake
#
#  Reserved for milestones 2 and 3 (Premiere importer and reframe effect).
#
#  The Adobe Premiere Pro and After Effects SDK headers are licensed for use
#  but NOT for redistribution, so this repository never contains them.  A
#  developer downloads the SDKs with an Adobe ID and points the two cache
#  variables at the extracted folders:
#
#     -DOSV_BUILD_PREMIERE=ON
#     -DOSV_PREMIERE_SDK_DIR=C:/Adobe/PremiereProSDK
#     -DOSV_AE_SDK_DIR=C:/Adobe/AfterEffectsSDK
#
#  Milestone 1 only validates the variables when the plug-ins are requested so
#  that a plain library build never depends on Adobe material.
# =============================================================================
if(NOT OSV_BUILD_PREMIERE)
  return()
endif()

if(NOT OSV_PREMIERE_SDK_DIR OR NOT EXISTS "${OSV_PREMIERE_SDK_DIR}")
  message(FATAL_ERROR
    "OSV_BUILD_PREMIERE=ON but OSV_PREMIERE_SDK_DIR does not point at a Premiere Pro SDK. "
    "Download the SDK from https://developer.adobe.com/premiere-pro/ and set the variable.")
endif()

if(NOT OSV_AE_SDK_DIR OR NOT EXISTS "${OSV_AE_SDK_DIR}")
  message(FATAL_ERROR
    "OSV_BUILD_PREMIERE=ON but OSV_AE_SDK_DIR does not point at an After Effects SDK. "
    "Download the SDK from https://developer.adobe.com/after-effects/ and set the variable.")
endif()

# The plug-in sub-projects (added in milestone 2) will consume these targets.
add_library(osv_premiere_sdk INTERFACE)
target_include_directories(osv_premiere_sdk INTERFACE
  "${OSV_PREMIERE_SDK_DIR}/Examples/Headers"
  "${OSV_AE_SDK_DIR}/Examples/Headers"
  "${OSV_AE_SDK_DIR}/Examples/Util"
)
message(STATUS "Premiere SDK: ${OSV_PREMIERE_SDK_DIR}")
message(STATUS "AE SDK      : ${OSV_AE_SDK_DIR}")
