# =============================================================================
#  OsvOnnxRuntime.cmake
#
#  Finds a prebuilt ONNX Runtime for the neural optical-flow backend
#  (src/osv/render/FlowBackendOnnx.cpp) and, when it is found, provides:
#
#    osv_onnxruntime        INTERFACE target: ORT include dir plus the
#                           OSV_HAVE_ONNXRUNTIME=1 definition.  Link it
#                           PRIVATE into the library that compiles the backend.
#    OSV_HAVE_ONNXRUNTIME   ON/OFF in the including scope.
#
#  When it is NOT found nothing is defined, FlowBackendOnnx.cpp compiles to
#  an empty translation unit, and the build succeeds exactly as before; the
#  Neural backend then reports "built without ONNX Runtime" at runtime.
#
#  WHY NO IMPORT LIBRARY IS LINKED
#  -------------------------------
#  The backend loads onnxruntime.dll itself, by FULL PATH, from the directory
#  of the module that contains it (LoadLibraryExW + GetProcAddress of
#  OrtGetApiBase).  Two reasons, both about code that runs inside a host:
#
#    * Windows 11 ships its own onnxruntime.dll (1.17, Windows ML) in
#      System32.  A plug-in loaded into Premiere does not have its own folder
#      on the DLL search path - the EXE's folder is Premiere's - so a normal
#      import would bind to that older, CUDA-less copy, or fail to start.
#    * A missing or broken DLL must be a runtime "unavailable", not a process
#      that refuses to launch.  An import-library link turns absence into a
#      loader error before main() or DllMain ever runs.
#
#  So only the headers are needed at compile time; the DLLs are staged next
#  to the binaries below so the runtime finds them where it looks.
#
#  WHERE IT LOOKS (first hit wins)
#  -------------------------------
#    OSV_ONNXRUNTIME_DIR cache variable
#    $ENV{ONNXRUNTIME_ROOT}
#    <source>/third_party/onnxruntime        (scripts/fetch_onnxruntime.ps1)
#
#  A candidate must hold include/onnxruntime_c_api.h and lib/onnxruntime.dll.
#
#  cuDNN 9 is looked for separately (OSV_CUDNN_DIR, $ENV{CUDNN_PATH}/bin,
#  <source>/third_party/cudnn/bin, NVIDIA's installer layout).  Its absence is
#  a warning, not an error: the backend then reports why it cannot run.
#
#  The model (models/*.onnx, from scripts/fetch_flow_model.py) is copied to
#  <bin>/models/ at BUILD time by the osv_flow_model target, so fetching it
#  after configuring needs only a rebuild, not a re-configure.
#
#  SCRIPT MODE: this same file is run with `cmake -P` by that target (see the
#  block at the top), which keeps the whole feature in one module.
# =============================================================================

# -----------------------------------------------------------------------------
#  cmake -P mode: copy models/*.onnx into the runtime directory.
#  Arguments: -DOSV_MODEL_SRC=<dir> -DOSV_MODEL_DST=<dir>
# -----------------------------------------------------------------------------
if(CMAKE_SCRIPT_MODE_FILE)
  if(NOT OSV_MODEL_SRC OR NOT OSV_MODEL_DST)
    message(FATAL_ERROR "OsvOnnxRuntime.cmake -P needs OSV_MODEL_SRC and OSV_MODEL_DST")
  endif()
  file(GLOB _models "${OSV_MODEL_SRC}/*.onnx")
  if(NOT _models)
    # Not an error: the neural backend is optional and says so at runtime.
    return()
  endif()
  file(MAKE_DIRECTORY "${OSV_MODEL_DST}")
  foreach(_m IN LISTS _models)
    get_filename_component(_name "${_m}" NAME)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_m}" "${OSV_MODEL_DST}/${_name}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
      message(FATAL_ERROR "could not stage ${_m} into ${OSV_MODEL_DST}")
    endif()
  endforeach()
  return()
endif()

include_guard(GLOBAL)

option(OSV_ENABLE_ONNXRUNTIME "Build the neural flow backend when ONNX Runtime is found" ON)
set(OSV_ONNXRUNTIME_DIR "" CACHE PATH "Prebuilt ONNX Runtime root (holds include/ and lib/)")
set(OSV_CUDNN_DIR "" CACHE PATH "Directory containing cudnn64_9.dll (optional)")

set(OSV_HAVE_ONNXRUNTIME OFF)

if(NOT OSV_ENABLE_ONNXRUNTIME)
  message(STATUS "ONNX Runtime   : disabled (OSV_ENABLE_ONNXRUNTIME=OFF)")
  return()
endif()
if(NOT WIN32)
  # The loader in FlowBackendOnnx.cpp handles POSIX, but this module only
  # knows the Windows package layout; say so rather than half-configure.
  message(STATUS "ONNX Runtime   : not searched (only the Windows package layout is supported)")
  return()
endif()

# -----------------------------------------------------------------------------
#  Locate the package.
# -----------------------------------------------------------------------------
set(_osv_ort_root "")
foreach(_cand IN ITEMS "${OSV_ONNXRUNTIME_DIR}" "$ENV{ONNXRUNTIME_ROOT}" "${CMAKE_SOURCE_DIR}/third_party/onnxruntime")
  if(_cand AND EXISTS "${_cand}/include/onnxruntime_c_api.h" AND EXISTS "${_cand}/lib/onnxruntime.dll")
    set(_osv_ort_root "${_cand}")
    break()
  endif()
endforeach()

if(NOT _osv_ort_root)
  message(STATUS "ONNX Runtime   : not found - neural flow backend disabled "
                 "(run scripts/fetch_onnxruntime.ps1, or set OSV_ONNXRUNTIME_DIR)")
  return()
endif()

# The version is informational; the runtime checks the API level it needs.
set(_osv_ort_version "unknown")
if(EXISTS "${_osv_ort_root}/VERSION_NUMBER")
  file(READ "${_osv_ort_root}/VERSION_NUMBER" _osv_ort_version)
  string(STRIP "${_osv_ort_version}" _osv_ort_version)
endif()

add_library(osv_onnxruntime INTERFACE)
# SYSTEM: third-party headers must not trip this project's warning level.
target_include_directories(osv_onnxruntime SYSTEM INTERFACE "${_osv_ort_root}/include")
target_compile_definitions(osv_onnxruntime INTERFACE OSV_HAVE_ONNXRUNTIME=1)
set(OSV_HAVE_ONNXRUNTIME ON)

# -----------------------------------------------------------------------------
#  Stage runtime DLLs beside the binaries.
#
#  Hard links where possible (third_party/ and build/ normally share a
#  volume, and the CUDA provider plus cuDNN are over a gigabyte), falling
#  back to a copy.  A file is re-staged only when its size or timestamp
#  changed, so re-configuring is cheap.
# -----------------------------------------------------------------------------
set(_osv_stage_dir "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
if(NOT _osv_stage_dir)
  set(_osv_stage_dir "${CMAKE_BINARY_DIR}/bin")
endif()
file(MAKE_DIRECTORY "${_osv_stage_dir}")

function(_osv_stage_runtime_file SRC)
  get_filename_component(_name "${SRC}" NAME)
  set(_dst "${_osv_stage_dir}/${_name}")
  if(EXISTS "${_dst}")
    file(SIZE "${SRC}" _s1)
    file(SIZE "${_dst}" _s2)
    file(TIMESTAMP "${SRC}" _t1 "%Y%m%d%H%M%S" UTC)
    file(TIMESTAMP "${_dst}" _t2 "%Y%m%d%H%M%S" UTC)
    # A hard link shares the source's timestamp; a fallback copy is stamped
    # when it was made.  "Same size and not older" covers both without
    # re-copying a gigabyte of cuDNN on every configure.
    if(_s1 EQUAL _s2 AND NOT _t2 STRLESS _t1)
      return()
    endif()
    file(REMOVE "${_dst}")
  endif()
  file(CREATE_LINK "${SRC}" "${_dst}" COPY_ON_ERROR)
endfunction()

set(_osv_ort_dlls onnxruntime.dll onnxruntime_providers_shared.dll onnxruntime_providers_cuda.dll)
foreach(_dll IN LISTS _osv_ort_dlls)
  if(EXISTS "${_osv_ort_root}/lib/${_dll}")
    _osv_stage_runtime_file("${_osv_ort_root}/lib/${_dll}")
  endif()
endforeach()

set(_osv_ort_has_cuda OFF)
if(EXISTS "${_osv_ort_root}/lib/onnxruntime_providers_cuda.dll")
  set(_osv_ort_has_cuda ON)
endif()

# cuDNN: ORT's CUDA provider loads cudnn64_9.dll by name at session creation,
# and cuDNN in turn loads its sub-libraries.  Staging the whole family next
# to onnxruntime.dll is what makes that lookup succeed without touching PATH.
set(_osv_cudnn_dir "")
set(_osv_cudnn_candidates "${OSV_CUDNN_DIR}")
if(DEFINED ENV{CUDNN_PATH})
  file(TO_CMAKE_PATH "$ENV{CUDNN_PATH}" _cudnn_env)
  list(APPEND _osv_cudnn_candidates "${_cudnn_env}/bin")
  file(GLOB _cudnn_env_sub LIST_DIRECTORIES true "${_cudnn_env}/bin/12.*")
  list(APPEND _osv_cudnn_candidates ${_cudnn_env_sub})
endif()
list(APPEND _osv_cudnn_candidates "${CMAKE_SOURCE_DIR}/third_party/cudnn/bin")
# NVIDIA's cuDNN 9 Windows installer: Program Files/NVIDIA/CUDNN/v9.x/bin/12.x
file(GLOB _cudnn_installed LIST_DIRECTORIES true "C:/Program Files/NVIDIA/CUDNN/v9.*/bin/12.*")
list(SORT _cudnn_installed ORDER DESCENDING)
list(APPEND _osv_cudnn_candidates ${_cudnn_installed})
foreach(_cand IN LISTS _osv_cudnn_candidates)
  if(_cand AND EXISTS "${_cand}/cudnn64_9.dll")
    set(_osv_cudnn_dir "${_cand}")
    break()
  endif()
endforeach()

if(_osv_cudnn_dir)
  file(GLOB _osv_cudnn_dlls "${_osv_cudnn_dir}/cudnn*64_9.dll")
  foreach(_dll IN LISTS _osv_cudnn_dlls)
    _osv_stage_runtime_file("${_dll}")
  endforeach()
elseif(_osv_ort_has_cuda)
  message(WARNING "ONNX Runtime has a CUDA provider but no cuDNN 9 was found, so the neural flow backend "
                  "will report itself unavailable unless cudnn64_9.dll is on PATH at runtime. "
                  "Run scripts/fetch_onnxruntime.ps1 or set OSV_CUDNN_DIR.")
endif()

# -----------------------------------------------------------------------------
#  Stage the model at build time (see the script-mode block above).
# -----------------------------------------------------------------------------
add_custom_target(osv_flow_model ALL
  COMMAND "${CMAKE_COMMAND}"
          "-DOSV_MODEL_SRC=${CMAKE_SOURCE_DIR}/models"
          "-DOSV_MODEL_DST=${_osv_stage_dir}/models"
          -P "${CMAKE_CURRENT_LIST_FILE}"
  COMMENT "Staging the neural flow model (if present) into ${_osv_stage_dir}/models"
  VERBATIM)
set_target_properties(osv_flow_model PROPERTIES FOLDER "osv")

set(_osv_cudnn_msg "not found")
if(_osv_cudnn_dir)
  set(_osv_cudnn_msg "${_osv_cudnn_dir}")
endif()
message(STATUS "ONNX Runtime   : ${_osv_ort_version} at ${_osv_ort_root} (CUDA provider: ${_osv_ort_has_cuda}; cuDNN: ${_osv_cudnn_msg})")
