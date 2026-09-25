# =============================================================================
#  OpenOSV - independent, open-source DJI Osmo 360 (.OSV) toolkit
#
#  Top-level build script.  Everything is opt-in through cache options so the
#  same tree builds a CPU-only library on a laptop, a CUDA + OpenCL build on a
#  workstation, and (later) the Premiere Pro plug-ins when an SDK path is given.
#
#  Layout (see docs/ARCHITECTURE.md):
#    include/osv/<module>   public headers (one class per header, Doxygen'd)
#    src/osv/<module>       implementation + per-module CMakeLists.txt
#    tools/osvtool          command line front end
#    tests                  Catch2 v3 test-suite + golden data
# =============================================================================
cmake_minimum_required(VERSION 3.28)

project(openosv
        VERSION 0.2.1
        DESCRIPTION "Independent open-source DJI Osmo 360 .OSV parser, stitcher, reframer and HDR colour pipeline"
        HOMEPAGE_URL "https://github.com/Kemerd/OpenOSV"
        LANGUAGES C CXX)

# -----------------------------------------------------------------------------
#  Build options
# -----------------------------------------------------------------------------
option(OSV_ENABLE_CUDA     "Build the CUDA renderer backend"                    ON)
option(OSV_ENABLE_OPENCL   "Build the OpenCL renderer backend"                  ON)
option(OSV_ENABLE_METAL    "Build the Metal renderer backend (macOS only)"      ON)
option(OSV_BUILD_TESTS     "Build the Catch2 test-suite"                        ON)
option(OSV_BUILD_TOOLS     "Build the osvtool command line utility"             ON)
option(OSV_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"               OFF)
option(OSV_BUILD_PREMIERE  "Build the Premiere Pro plug-ins (needs SDK paths)"  OFF)
# The OpenFX plug-ins for DaVinci Resolve (plugins/ofx, docs/RESOLVE.md).  They
# need no Adobe SDK, so they build in every configuration, CI included.
option(OSV_BUILD_OFX       "Build the OpenFX plug-ins for DaVinci Resolve"     ON)

# Out-of-tree SDK locations.  The Adobe SDK headers are NOT redistributable and
# are therefore never committed; the user points these at a local download.
set(OSV_PREMIERE_SDK_DIR "" CACHE PATH "Premiere Pro C++ SDK root (out of tree)")
set(OSV_AE_SDK_DIR       "" CACHE PATH "After Effects C++ SDK root (out of tree)")

# Sample clip used by the [sample] tagged tests.  Tests SKIP cleanly when the
# file is absent, so CI machines without footage still exercise everything that
# can be verified from synthetic fixtures.
set(OSV_SAMPLE_FILE "${CMAKE_SOURCE_DIR}/example_footage_dlogm.OSV"
    CACHE FILEPATH "Sample .OSV clip for the [sample] tests")
set(OSV_SAMPLE_LRF_FILE "${CMAKE_SOURCE_DIR}/example_footage_dlogm.LRF"
    CACHE FILEPATH "Sample .LRF proxy clip for the [sample] tests")

# -----------------------------------------------------------------------------
#  Language standard and global settings
# -----------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Keep every binary in one predictable place so the FFmpeg DLLs copied by the
# vcpkg "applocal" step sit next to the executables that need them.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(OsvWarnings)
include(OsvFFmpeg)
include(OsvPremiereSdk)
include(EmbedKernel)

if(OSV_ENABLE_CUDA)
  include(OsvCuda)
endif()
if(OSV_ENABLE_OPENCL)
  include(OsvOpenCL)
endif()
# Metal exists on macOS only; elsewhere the option is inert.
if(OSV_ENABLE_METAL AND APPLE)
  include(OsvMetal)
endif()

# -----------------------------------------------------------------------------
#  Third-party packages (all via vcpkg manifest; see vcpkg.json)
# -----------------------------------------------------------------------------
find_package(fmt CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(tinyexr CONFIG REQUIRED)

# -----------------------------------------------------------------------------
#  Version header generated from the project() version
# -----------------------------------------------------------------------------
configure_file("${CMAKE_SOURCE_DIR}/include/osv/core/Version.h.in"
               "${CMAKE_BINARY_DIR}/generated/osv/core/Version.h" @ONLY)

# -----------------------------------------------------------------------------
#  Library modules (strict one-way dependency chain, see src/CMakeLists.txt)
# -----------------------------------------------------------------------------
add_subdirectory(src)

if(OSV_BUILD_TOOLS)
  add_subdirectory(tools/osvtool)
endif()

if(OSV_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

# Premiere Pro plug-ins (importer .prm + reframe effect .aex).  Requires the
# Adobe SDK paths validated by cmake/OsvPremiereSdk.cmake; see docs/PREMIERE.md.
if(OSV_BUILD_PREMIERE)
  add_subdirectory(plugins)
endif()

# OpenFX plug-ins for DaVinci Resolve (OpenOSV.ofx.bundle), and the tests that
# drive the built bundle through a mock OpenFX host.  After plugins/ because
# the bundle reuses, when present, the importer's pack kernel library.
if(OSV_BUILD_OFX)
  add_subdirectory(plugins/ofx)
  if(OSV_BUILD_TESTS)
    add_subdirectory(tests/ofx)
  endif()
endif()

# -----------------------------------------------------------------------------
#  Summary
# -----------------------------------------------------------------------------
message(STATUS "OpenOSV ${PROJECT_VERSION}")
message(STATUS "  CUDA backend   : ${OSV_ENABLE_CUDA}")
message(STATUS "  OpenCL backend : ${OSV_ENABLE_OPENCL}")
if(APPLE)
  message(STATUS "  Metal backend  : ${OSV_ENABLE_METAL}")
endif()
message(STATUS "  Tests          : ${OSV_BUILD_TESTS}")
message(STATUS "  Tools          : ${OSV_BUILD_TOOLS}")
message(STATUS "  Sample clip    : ${OSV_SAMPLE_FILE}")
message(STATUS "  Premiere plugs : ${OSV_BUILD_PREMIERE}")
message(STATUS "  OpenFX plugs   : ${OSV_BUILD_OFX}")
