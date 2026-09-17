# =============================================================================
#  osv_premiere_mockhost - the fake Premiere Pro host (docs/PREMIERE.md,
#  "Verification").
#
#  Deliberately Catch2-free so a plug-in, a benchmark or a scratch program can
#  link it as well.  It compiles against both SDKs (the AE side is needed for
#  PF_InData / PF_EffectWorld and the PF suites) and against the library for
#  nothing at all - the mock host owns only host-side state.
#
#  MockGpu.cpp implements the GPU Device Suite on a real CUDA driver-API
#  context; without OSV_ENABLE_CUDA it compiles to a stub that reports "no
#  CUDA in this build", so the file is always in the target and the CUDA
#  dependency is added conditionally.
# =============================================================================
add_library(osv_premiere_mockhost STATIC
  MockAe.cpp
  MockDrawbot.cpp
  MockGpu.cpp
  MockHost.cpp
  MockHost.h
  MockHostImpl.h
  MockPPix.cpp
  MockSuites.cpp
)
add_library(osv::premiere_mockhost ALIAS osv_premiere_mockhost)

target_include_directories(osv_premiere_mockhost PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

target_link_libraries(osv_premiere_mockhost PUBLIC
  osv_warnings
  osv_ae_sdk        # brings osv_premiere_sdk with it
)

# /MD like every plug-in module and the test executables that link this.
set_target_properties(osv_premiere_mockhost PROPERTIES
  MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
  POSITION_INDEPENDENT_CODE ON
  FOLDER "tests"
)

# The CUDA driver API (cuda.lib / cuda.h), used through the driver API only -
# no cudart, exactly like the effect's GPU path will.
if(OSV_ENABLE_CUDA AND TARGET CUDA::cuda_driver)
  target_link_libraries(osv_premiere_mockhost PUBLIC CUDA::cuda_driver)
  target_compile_definitions(osv_premiere_mockhost PUBLIC OSV_MOCK_HAVE_CUDA=1)
else()
  target_compile_definitions(osv_premiere_mockhost PUBLIC OSV_MOCK_HAVE_NO_CUDA=1)
endif()
