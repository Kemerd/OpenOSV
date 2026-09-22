// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// FlowBackendOnnx.cpp - the neural flow backend: SEA-RAFT through ONNX Runtime.
//
// THE MODEL
// ---------
//     Z. Wang, L. Lipson, J. Deng, "SEA-RAFT: Simple, Efficient, Accurate
//     RAFT for Optical Flow", ECCV 2024.  https://arxiv.org/abs/2405.14793
//     Code and weights BSD-3-Clause (princeton-vl/SEA-RAFT; weights from the
//     authors' own Hugging Face organisation, MemorySlices).
//
// Small variant, 8.9 M parameters, 4 refinement iterations, exported by
// scripts/fetch_flow_model.py to models/searaft_s_gray.onnx.  It was chosen
// over NeuFlowV2 and OpenCV's RAFT export because both of those bake spatial
// constants into their graphs and fail at any resolution but one, and the
// overlap band here is a wide, short strip whose size depends on the clip.
// SEA-RAFT is fully convolutional; the export keeps height and width dynamic.
//
// THE GRAPH'S CONTRACT (fixed by the export script, relied on below)
// -----------------------------------------------------------------
//   inputs   image_a, image_b : float32 [N, 1, H, W], values in [0, 1]
//   output   flow             : float32 [N, 2, H, W], pixels, (u, v) at p
//                               means content at p in image_a is at p + (u, v)
//                               in image_b - DisFlow.h's convention exactly
//   H, W     multiples of 8 (the upstream padder is not in the graph) and at
//            least 128 (the 4-level correlation pyramid halves the 1/8-scale
//            feature map four times and fails below 16 rows)
//
// WHAT THIS FILE ADDS AROUND THE NETWORK, AND WHY
// -----------------------------------------------
//  1. A working resolution.  The real overlap band is ~2048 x 68 - too short
//     for the network even to run.  Padding it to 128 rows with replicated
//     edges runs, but was MEASURED to be poor: mean end-point error 4.1 px
//     over a grid of shifts, worst 26 px, because replicated rows carry no
//     vertical texture.  Upscaling isotropically until the band is
//     kMinWorkingRows tall, running there and scaling the flow back down was
//     measured on the same grid at 0.054 px mean, 0.26 px worst.  The network
//     simply needs enough rows in its 1/8-scale feature map.
//  2. Tiling.  SEA-RAFT's correlation volume is all-pairs: its memory grows
//     with the SQUARE of the tile area (a 7680 x 768 tile would need ~45 GB).
//     Work is therefore split into uniform tiles bounded by
//     FlowBackendParams::maxTileEdge on each axis AND by kMaxTilePixels in
//     area, overlapped by tileOverlapPx and cross-faded with a raised cosine
//     across the whole overlap.  Uniform size matters for speed too: cuDNN
//     tunes per input shape, so one shape per band means one tuning.
//  3. Both directions in one call.  Each tile runs as a batch of two,
//     (a, b) and (b, a), so the backward field costs one kernel launch
//     sequence rather than a second pass over every tile.
//  4. The same forward-backward consistency mask disFlowBidirectional()
//     computes, bit for bit in its semantics, so nothing downstream needs
//     to know which backend produced a field.
//
// LOADING, AND WHY IT IS DONE BY HAND
// -----------------------------------
// onnxruntime.dll is loaded at runtime, by full path, from the directory of
// the module this code is linked into (osvtool.exe, osv_tests.exe, or a
// plug-in), rather than through an import library.  Windows 11 ships an
// older, CUDA-less onnxruntime.dll in System32, and a plug-in's own folder is
// not on the DLL search path inside its host, so a by-name load would bind to
// the wrong copy - or fail to start the host at all.  cmake/OsvOnnxRuntime.cmake
// stages the DLLs beside the binaries.
//
// ONNX Runtime's CUDA provider then needs the CUDA 12 runtime, cuBLAS and
// cuDNN 9.  Those are pre-loaded here by full path from a short list of
// places (beside this module first) so the provider's by-name lookups
// resolve to them; see preloadCudaDependencies().
//
// FAILURE IS A NORMAL OUTCOME
// ---------------------------
// A missing model, a missing DLL, no CUDA device, a model with the wrong
// signature - each yields a backend whose isAvailable() is false and whose
// info().detail says which one, never an exception and never a crash.  There
// is deliberately NO CPU fallback: measured at ~2.7 s per 2048 x 256 tile, a
// CPU-only neural backend would be ~150x slower than on the GPU and much
// slower than DIS, and FlowBackendKind::Auto would pick it.
//
// LIFETIME
// --------
// The ONNX Runtime environment and the per-model sessions are cached for the
// life of the process and intentionally never destroyed.  computeFlow()
// builds a backend per call, and creating a session costs seconds; and
// tearing a CUDA context down from static destructors or DllMain - which is
// where process exit would run it - is a well-known way to deadlock or crash
// a host on the way out.  The OS reclaims everything at exit.

#include "osv/render/FlowBackend.h"

#if defined(OSV_HAVE_ONNXRUNTIME)

#if !defined(_WIN32)
#error "The ONNX Runtime loader in FlowBackendOnnx.cpp is Windows-only; configure without OSV_HAVE_ONNXRUNTIME."
#endif

#include "osv/core/Log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <onnxruntime_c_api.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace osv::render {

/// Declared in FlowBackend.cpp, which is the only caller.
std::unique_ptr<FlowBackend> makeOnnxFlowBackend(const FlowBackendParams& params);

namespace {

namespace fs = std::filesystem;

// ===========================================================================
//  Constants.  Each one is either part of the model's contract or a measured
//  operating point; none is a tuning knob pulled from the air.
// ===========================================================================

/// File name looked for in `<module dir>/models/` when no explicit path is
/// given.  Written by scripts/fetch_flow_model.py.
constexpr wchar_t kDefaultModelFile[] = L"searaft_s_gray.onnx";

/// Oldest ONNX Runtime C API level providing every function this file calls
/// (SessionOptionsAppendExecutionProvider_CUDA_V2 arrived in API 11).  The
/// API table is append-only, so asking for this rather than the header's
/// ORT_API_VERSION lets a slightly older bundled runtime still work.
constexpr std::uint32_t kOrtApiLevel = 11;

/// Tensor edges must be multiples of this (no padder in the graph).
constexpr std::uint32_t kModelAlign = 8;

/// Smallest tensor edge the graph can run at (see the file header).
constexpr std::uint32_t kMinModelEdge = 128;

/// Bands shorter than this are upscaled isotropically until they are this
/// tall before inference.
///
/// The network needs enough rows in its 1/8-scale feature map.  Measured on
/// 2048 x 68 bands over 60 shifts |dx| <= 12, |dy| <= 4 and three textures
/// (mean / worst end-point error, px):
///     pad to 128 rows   4.09 / 26.4
///     scale to 128      0.48 /  2.59
///     scale to 192      0.054 / 0.26
///     scale to 256      0.036 / 0.22
/// 192 rather than 256 because the last step costs a whole extra tile: on an
/// RTX 5090 the real 2048 x 68 band takes 91 ms at 192 rows and 175 ms at
/// 256 (DIS: 104 ms), for an accuracy difference of a few hundredths of a
/// pixel.  A band already this tall runs at native size (a 200-row band:
/// 0.133 px mean).
constexpr std::uint32_t kMinWorkingRows = 192;

/// Ceiling on one tile's area, in working pixels.
///
/// The all-pairs correlation volume is (H/8 * W/8)^2 floats and the pyramid
/// adds a third again: at 2048 x 256 that is ~0.36 GB per image pair, twice
/// that for the two directions batched together.  The cap keeps a single
/// inference well under a gigabyte of GPU memory whatever maxTileEdge says,
/// which matters inside a host that is also using the GPU.
constexpr std::uint64_t kMaxTilePixels = 2048ull * 256ull;

/// Upper clamp on FlowBackendParams::maxTileEdge; anything larger is a
/// corrupt preference rather than a request.
constexpr std::uint32_t kMaxTileEdgeCeiling = 8192;

/// Largest input edge accepted.  A band is a few thousand pixels; past this
/// the size arithmetic below is guarding against garbage, not serving a use.
constexpr std::uint32_t kMaxInputEdge = 1u << 16;

/// After this many inference failures in a row the engine reports itself
/// unavailable, so a lost GPU costs three fallbacks rather than one per frame.
constexpr int kMaxConsecutiveFailures = 3;

// ===========================================================================
//  Small platform helpers.
// ===========================================================================

/// UTF-8 std::string -> filesystem path (UTF-16 on Windows).
fs::path pathFromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

/// Filesystem path -> UTF-8 std::string, for messages.
std::string utf8(const fs::path& p) {
    const std::u8string u = p.u8string();
    return std::string(u.begin(), u.end());
}

/// Wide string -> UTF-8.  Used for Windows error text.
std::string utf8(const std::wstring& w) {
    if (w.empty()) {
        return {};
    }
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

/// Human text for a Win32 error code, trimmed of the trailing newline.
std::string win32ErrorText(DWORD code) {
    wchar_t* buf = nullptr;
    const DWORD n =
        FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       nullptr, code, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::string text;
    if (n > 0 && buf != nullptr) {
        text = utf8(std::wstring(buf, n));
    }
    if (buf != nullptr) {
        LocalFree(buf);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text.empty() ? ("Win32 error " + std::to_string(code)) : (text + " (" + std::to_string(code) + ")");
}

/// Read an environment variable as a path; empty when unset.
fs::path envPath(const wchar_t* name) {
    // _wdupenv_s rather than _wgetenv: the latter is flagged unsafe by MSVC
    // and returns a pointer into the CRT's environment block.
    wchar_t* value = nullptr;
    std::size_t len = 0;
    if (_wdupenv_s(&value, &len, name) != 0 || value == nullptr) {
        return {};
    }
    fs::path out(value);
    std::free(value);
    return out;
}

/// Anchor whose address identifies the module this file is linked into.
const char kModuleAnchor = 0;

/// Directory of the module (EXE or DLL) that contains this code.
///
/// Not the process EXE: inside Premiere the EXE is Premiere, and the files
/// this backend needs sit beside the plug-in.
fs::path thisModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&kModuleAnchor), &self) ||
        self == nullptr) {
        return {};
    }
    // Grow the buffer until the name fits; long-path systems exceed MAX_PATH.
    std::wstring buf(MAX_PATH, L'\0');
    for (int attempt = 0; attempt < 8; ++attempt) {
        const DWORD n = GetModuleFileNameW(self, buf.data(), static_cast<DWORD>(buf.size()));
        if (n == 0) {
            return {};
        }
        if (n < buf.size()) {
            buf.resize(n);
            return fs::path(buf).parent_path();
        }
        buf.resize(buf.size() * 2);
    }
    return {};
}

/// Suppress the "missing DLL" critical-error dialog for the lifetime of the
/// object.  Harmless in a console tool, essential inside a host: a modal box
/// on a render thread is a hang, not a message.
class QuietDllErrors {
public:
    QuietDllErrors() { m_ok = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &m_previous) != 0; }
    ~QuietDllErrors() {
        if (m_ok) {
            SetThreadErrorMode(m_previous, nullptr);
        }
    }
    QuietDllErrors(const QuietDllErrors&) = delete;
    QuietDllErrors& operator=(const QuietDllErrors&) = delete;

private:
    DWORD m_previous = 0;
    bool m_ok = false;
};

/// Load a DLL by full path so ITS dependencies resolve from its own folder.
HMODULE loadFullPath(const fs::path& p, std::string* error) {
    std::error_code ec;
    if (p.empty() || !fs::is_regular_file(p, ec)) {
        if (error) {
            *error = "not found: " + utf8(p);
        }
        return nullptr;
    }
    HMODULE h = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (h == nullptr && error) {
        *error = utf8(p) + ": " + win32ErrorText(GetLastError());
    }
    return h;
}

// ===========================================================================
//  CUDA dependency resolution.
// ===========================================================================

/// CUDA 12 runtime DLLs the gpu_cuda12 ONNX Runtime package's CUDA provider
/// imports statically (checked with dumpbin on 1.30.0).  A cuda13 package
/// imports the *_13 names instead; its runtime must then sit beside
/// onnxruntime.dll, where the provider's own lookup finds it without help.
constexpr std::array<const wchar_t*, 3> kCudaRuntimeDlls = {L"cudart64_12.dll", L"cublasLt64_12.dll",
                                                            L"cublas64_12.dll"};

/// cuDNN 9's entry DLL.  The provider loads it by name when a session is
/// created; cuDNN then loads its cudnn_*64_9.dll sub-libraries.
constexpr const wchar_t* kCudnnDll = L"cudnn64_9.dll";

/// Sub-directories of `root` whose name starts with `prefix`, newest-looking
/// first (reverse lexicographic, which orders v12.9 before v12.6).
std::vector<fs::path> subdirsWithPrefix(const fs::path& root, const std::wstring& prefix) {
    std::vector<fs::path> out;
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root, ec)) {
        return out;
    }
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring name = it->path().filename().wstring();
        if (it->is_directory(ec) && name.rfind(prefix, 0) == 0) {
            out.push_back(it->path());
        }
    }
    std::sort(out.begin(), out.end(), [](const fs::path& x, const fs::path& y) { return x.wstring() > y.wstring(); });
    return out;
}

/// Where the CUDA 12 runtime may live, in priority order.
///
/// CUDA_PATH before the default search order on purpose: PATH commonly still
/// lists an OLDER 12.x toolkit (this project's own dev machine lists 12.6
/// ahead of 12.9), and a provider built against a newer 12.x can import
/// symbols an older cudart64_12.dll does not export.
std::vector<fs::path> cudaRuntimeDirs(const fs::path& moduleDir) {
    std::vector<fs::path> dirs{moduleDir};
    if (const fs::path cp = envPath(L"CUDA_PATH"); !cp.empty()) {
        dirs.push_back(cp / "bin");
    }
    const fs::path pf = envPath(L"ProgramFiles");
    for (const fs::path& d : subdirsWithPrefix(pf / "NVIDIA GPU Computing Toolkit" / "CUDA", L"v12.")) {
        dirs.push_back(d / "bin");
    }
    return dirs;
}

/// Where cuDNN 9 may live, in priority order.
std::vector<fs::path> cudnnDirs(const fs::path& moduleDir) {
    std::vector<fs::path> dirs{moduleDir};
    if (const fs::path cp = envPath(L"CUDNN_PATH"); !cp.empty()) {
        dirs.push_back(cp / "bin");
        for (const fs::path& d : subdirsWithPrefix(cp / "bin", L"12.")) {
            dirs.push_back(d);
        }
    }
    // NVIDIA's cuDNN 9 installer: Program Files\NVIDIA\CUDNN\v9.x\bin\12.x
    const fs::path pf = envPath(L"ProgramFiles");
    for (const fs::path& ver : subdirsWithPrefix(pf / "NVIDIA" / "CUDNN", L"v9.")) {
        for (const fs::path& d : subdirsWithPrefix(ver / "bin", L"12.")) {
            dirs.push_back(d);
        }
    }
    if (const fs::path cp = envPath(L"CUDA_PATH"); !cp.empty()) {
        dirs.push_back(cp / "bin");  // where people often copy it by hand
    }
    return dirs;
}

/// Make sure `name` is loaded in this process, trying `dirs` in order and
/// then the default search order.  Once a DLL is loaded, every later by-name
/// load of the same file name - including ONNX Runtime's - binds to it.
///
/// Returns true when loaded; `fromDir` is then the directory it came from
/// (left empty when it was already loaded or came from the default search).
/// Returns false with `why` describing what was tried.
bool ensureLoaded(const wchar_t* name, const std::vector<fs::path>& dirs, fs::path& fromDir, std::string& why) {
    if (GetModuleHandleW(name) != nullptr) {
        return true;
    }
    std::string misses;
    for (const fs::path& d : dirs) {
        std::error_code ec;
        const fs::path candidate = d / name;
        if (d.empty() || !fs::is_regular_file(candidate, ec)) {
            continue;
        }
        std::string err;
        if (loadFullPath(candidate, &err) != nullptr) {
            fromDir = d;
            return true;
        }
        // Present but unloadable (wrong architecture, missing dependency):
        // remember it, it is the most useful thing to tell the user.
        misses += (misses.empty() ? "" : "; ") + err;
    }
    // Last resort: the standard search order (exe dir, System32, PATH).
    if (LoadLibraryExW(name, nullptr, 0) != nullptr) {
        return true;
    }
    why = utf8(std::wstring(name)) +
          " not found beside the module, in CUDA_PATH/CUDNN_PATH, in Program Files "
          "or on PATH" +
          (misses.empty() ? std::string() : " (" + misses + ")");
    return false;
}

/// Pre-load everything the CUDA provider will ask for.
///
/// For cuDNN, every sibling cudnn_*64_9.dll is loaded too: cuDNN resolves
/// its sub-libraries by name at first use, and inside a host none of the
/// directories it would search is ours.
///
/// Returns "" on success, else a description of what is missing.  Never
/// fatal on its own: the provider's own error is reported alongside.
std::string preloadCudaDependencies(const fs::path& moduleDir) {
    QuietDllErrors quiet;
    std::string problems;
    const std::vector<fs::path> rtDirs = cudaRuntimeDirs(moduleDir);
    for (const wchar_t* dll : kCudaRuntimeDlls) {
        fs::path from;
        std::string why;
        if (!ensureLoaded(dll, rtDirs, from, why)) {
            problems += (problems.empty() ? "" : "; ") + why;
        }
    }
    fs::path cudnnFrom;
    std::string why;
    if (!ensureLoaded(kCudnnDll, cudnnDirs(moduleDir), cudnnFrom, why)) {
        problems += (problems.empty() ? "" : "; ") + why;
    } else if (!cudnnFrom.empty()) {
        std::error_code ec;
        for (fs::directory_iterator it(cudnnFrom, ec), end; !ec && it != end; it.increment(ec)) {
            const std::wstring n = it->path().filename().wstring();
            if (n.rfind(L"cudnn_", 0) == 0 && n.size() > 8 && n.substr(n.size() - 8) == L"64_9.dll" &&
                GetModuleHandleW(n.c_str()) == nullptr) {
                std::string err;
                if (loadFullPath(it->path(), &err) == nullptr) {
                    log::debug("neural flow: could not pre-load {}: {}", log::safe(utf8(it->path())), log::safe(err));
                }
            }
        }
        log::debug("neural flow: cuDNN from {}", log::safe(utf8(cudnnFrom)));
    }
    return problems;
}

// ===========================================================================
//  ONNX Runtime: the process-wide environment.
// ===========================================================================

/// Take ownership of an OrtStatus: returns "" for success (null), otherwise
/// the message, and releases the status either way.
std::string takeStatus(const OrtApi* api, OrtStatus* status) {
    if (status == nullptr) {
        return {};
    }
    std::string msg = api->GetErrorMessage(status);
    api->ReleaseStatus(status);
    return msg.empty() ? std::string("unspecified ONNX Runtime error") : msg;
}

/// ONNX Runtime's log lines, routed into ours.  Its WARNINGs are routine
/// (e.g. "Memcpy nodes are added to the graph") so they go to debug; only
/// ERROR and FATAL are worth a user's attention.
void ORT_API_CALL ortLogSink(void* /*param*/, OrtLoggingLevel severity, const char* category, const char* /*logid*/,
                             const char* /*location*/, const char* message) {
    const std::string text = log::safe(message != nullptr ? message : "");
    const std::string cat = log::safe(category != nullptr ? category : "");
    if (severity >= ORT_LOGGING_LEVEL_ERROR) {
        log::warn("onnxruntime [{}]: {}", cat, text);
    } else {
        log::debug("onnxruntime [{}]: {}", cat, text);
    }
}

/// The loaded runtime, created once per process and never destroyed.
struct OrtRuntime {
    const OrtApi* api = nullptr;
    OrtEnv* env = nullptr;
    std::string version;       ///< e.g. "1.30.0"
    fs::path moduleDir;        ///< where onnxruntime.dll came from
    std::string cudaProblems;  ///< dependency diagnosis, "" when all found
    std::string failure;       ///< non-empty when the runtime is unusable
};

/// Initialise the runtime on first use.  Thread-safe; the result, success or
/// failure, is final for the process - DLLs do not appear mid-session.
const OrtRuntime& ortRuntime() {
    static OrtRuntime* runtime = [] {
        auto* rt = new OrtRuntime();  // intentionally leaked, see LIFETIME
        rt->moduleDir = thisModuleDir();
        if (rt->moduleDir.empty()) {
            rt->failure = "could not determine the directory of this module";
            return rt;
        }
        QuietDllErrors quiet;
        std::string err;
        const fs::path dll = rt->moduleDir / L"onnxruntime.dll";
        HMODULE ort = loadFullPath(dll, &err);
        if (ort == nullptr) {
            rt->failure = "ONNX Runtime could not be loaded (" + err + ")";
            return rt;
        }
        using GetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();
        auto getApiBase = reinterpret_cast<GetApiBaseFn>(GetProcAddress(ort, "OrtGetApiBase"));
        if (getApiBase == nullptr) {
            rt->failure = utf8(dll) + " does not export OrtGetApiBase; it is not ONNX Runtime";
            return rt;
        }
        const OrtApiBase* base = getApiBase();
        if (base == nullptr) {
            rt->failure = "OrtGetApiBase returned null";
            return rt;
        }
        rt->version = base->GetVersionString != nullptr ? base->GetVersionString() : "unknown";
        rt->api = base->GetApi(kOrtApiLevel);
        if (rt->api == nullptr) {
            rt->failure = "ONNX Runtime " + rt->version + " does not provide C API level " +
                          std::to_string(kOrtApiLevel) + " (too old)";
            return rt;
        }
        err = takeStatus(rt->api, rt->api->CreateEnvWithCustomLogger(ortLogSink, nullptr, ORT_LOGGING_LEVEL_WARNING,
                                                                     "openosv", &rt->env));
        if (!err.empty() || rt->env == nullptr) {
            rt->failure = "ONNX Runtime environment could not be created: " + err;
            rt->api = nullptr;
            return rt;
        }
        // Resolve CUDA dependencies now, once, so each session creation does
        // not repeat the directory walk.
        rt->cudaProblems = preloadCudaDependencies(rt->moduleDir);
        log::info("neural flow: ONNX Runtime {} loaded from {}", log::safe(rt->version),
                  log::safe(utf8(rt->moduleDir)));
        return rt;
    }();
    return *runtime;
}

// ===========================================================================
//  One loaded model on one device.
// ===========================================================================

/// RAII for an OrtValue.
class ValueGuard {
public:
    explicit ValueGuard(const OrtApi* api)
        : m_api(api) {}
    ~ValueGuard() {
        if (m_value != nullptr) {
            m_api->ReleaseValue(m_value);
        }
    }
    ValueGuard(const ValueGuard&) = delete;
    ValueGuard& operator=(const ValueGuard&) = delete;
    OrtValue** put() { return &m_value; }
    [[nodiscard]] OrtValue* get() const { return m_value; }

private:
    const OrtApi* m_api;
    OrtValue* m_value = nullptr;
};

/// A session plus what is needed to feed it.  Created by EngineCache only.
class OrtEngine {
public:
    /// Build an engine for `model`.  Never throws; a failure is recorded in
    /// the returned engine and reported by detail().
    static std::shared_ptr<OrtEngine> create(const fs::path& model);

    /// True while the engine can run.
    [[nodiscard]] bool usable() const {
        return m_session != nullptr && m_consecutiveFailures.load() < kMaxConsecutiveFailures;
    }

    /// Model and device when usable, otherwise why not.
    [[nodiscard]] std::string detail() const {
        std::lock_guard<std::mutex> lock(m_detailMutex);
        return m_detail;
    }

    /// Run a batch.  `a` and `b` are [n, 1, h, w]; `flow` receives
    /// [n, 2, h, w].  Thread-safe: runs are serialised, because concurrent
    /// runs would each hold a correlation volume and multiply peak memory
    /// while the GPU is already saturated by one.
    Status run(const std::vector<float>& a, const std::vector<float>& b, std::int64_t n, std::int64_t h, std::int64_t w,
               std::vector<float>& flow);

    OrtEngine(const OrtEngine&) = delete;
    OrtEngine& operator=(const OrtEngine&) = delete;
    ~OrtEngine() {
        // Only reached when a model file changed and the old engine's last
        // user let go - normal runtime, not process exit (see LIFETIME).
        if (m_api != nullptr) {
            if (m_cpuInfo != nullptr) {
                m_api->ReleaseMemoryInfo(m_cpuInfo);
            }
            if (m_session != nullptr) {
                m_api->ReleaseSession(m_session);
            }
        }
    }

private:
    OrtEngine() = default;

    /// Replace the text detail() reports: the healthy description, or why
    /// the engine cannot run.
    void setDetail(std::string text) {
        std::lock_guard<std::mutex> lock(m_detailMutex);
        m_detail = std::move(text);
    }

    /// Check the graph is the one this file was written against.
    std::string validateSignature() const;

    const OrtApi* m_api = nullptr;
    OrtSession* m_session = nullptr;
    OrtMemoryInfo* m_cpuInfo = nullptr;
    std::mutex m_runMutex;
    mutable std::mutex m_detailMutex;
    std::string m_detail;
    std::string m_healthyDetail;
    std::atomic<int> m_consecutiveFailures{0};
};

std::string OrtEngine::validateSignature() const {
    const OrtApi* api = m_api;
    std::size_t nIn = 0;
    std::size_t nOut = 0;
    std::string err = takeStatus(api, api->SessionGetInputCount(m_session, &nIn));
    if (err.empty()) {
        err = takeStatus(api, api->SessionGetOutputCount(m_session, &nOut));
    }
    if (!err.empty()) {
        return err;
    }
    if (nIn != 2 || nOut != 1) {
        return "expected 2 inputs and 1 output, found " + std::to_string(nIn) + " and " + std::to_string(nOut);
    }
    OrtAllocator* alloc = nullptr;
    err = takeStatus(api, api->GetAllocatorWithDefaultOptions(&alloc));
    if (!err.empty() || alloc == nullptr) {
        return "no default allocator: " + err;
    }

    // Each input: float, rank 4, one channel.  Each output: float, rank 4,
    // two channels.  Dynamic dimensions report -1 and are accepted.
    struct Expect {
        bool input;
        std::size_t index;
        const char* name;
        std::int64_t channels;
    };
    const std::array<Expect, 3> expected = {{{true, 0, "image_a", 1}, {true, 1, "image_b", 1}, {false, 0, "flow", 2}}};
    for (const Expect& e : expected) {
        char* name = nullptr;
        err = takeStatus(api, e.input ? api->SessionGetInputName(m_session, e.index, alloc, &name)
                                      : api->SessionGetOutputName(m_session, e.index, alloc, &name));
        if (!err.empty()) {
            return err;
        }
        const std::string got = name != nullptr ? name : "";
        if (name != nullptr) {
            (void)takeStatus(api, api->AllocatorFree(alloc, name));
        }
        if (got != e.name) {
            return std::string(e.input ? "input " : "output ") + std::to_string(e.index) + " is '" + got +
                   "', expected '" + e.name + "'";
        }
        OrtTypeInfo* typeInfo = nullptr;
        err = takeStatus(api, e.input ? api->SessionGetInputTypeInfo(m_session, e.index, &typeInfo)
                                      : api->SessionGetOutputTypeInfo(m_session, e.index, &typeInfo));
        if (!err.empty() || typeInfo == nullptr) {
            return "no type info for '" + got + "': " + err;
        }
        const OrtTensorTypeAndShapeInfo* tensorInfo = nullptr;  // owned by typeInfo
        ONNXTensorElementDataType type = ONNX_TENSOR_ELEMENT_DATA_TYPE_UNDEFINED;
        std::size_t rank = 0;
        std::array<std::int64_t, 4> dims{};
        err = takeStatus(api, api->CastTypeInfoToTensorInfo(typeInfo, &tensorInfo));
        if (err.empty() && tensorInfo != nullptr) {
            err = takeStatus(api, api->GetTensorElementType(tensorInfo, &type));
        }
        if (err.empty() && tensorInfo != nullptr) {
            err = takeStatus(api, api->GetDimensionsCount(tensorInfo, &rank));
        }
        if (err.empty() && tensorInfo != nullptr && rank == 4) {
            err = takeStatus(api, api->GetDimensions(tensorInfo, dims.data(), dims.size()));
        }
        api->ReleaseTypeInfo(typeInfo);
        if (!err.empty()) {
            return "'" + got + "': " + err;
        }
        if (tensorInfo == nullptr || type != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || rank != 4) {
            return "'" + got + "' is not a float32 rank-4 tensor";
        }
        if (dims[1] != e.channels && dims[1] != -1) {
            return "'" + got + "' has " + std::to_string(dims[1]) + " channels, expected " + std::to_string(e.channels);
        }
    }
    return {};
}

std::shared_ptr<OrtEngine> OrtEngine::create(const fs::path& model) {
    std::shared_ptr<OrtEngine> engine(new OrtEngine());

    // The model first: it is the cheap check and by far the likeliest miss,
    // and checking it before touching ONNX Runtime means a machine without
    // the model never maps a gigabyte of CUDA libraries just to say so.
    std::error_code ec;
    if (model.empty()) {
        engine->setDetail("no model path could be determined");
        return engine;
    }
    if (!fs::is_regular_file(model, ec)) {
        engine->setDetail("model file not found: " + utf8(model) +
                          " (run scripts/fetch_flow_model.py, or set FlowBackendParams::modelPath)");
        return engine;
    }

    const OrtRuntime& rt = ortRuntime();
    if (!rt.failure.empty() || rt.api == nullptr || rt.env == nullptr) {
        engine->setDetail(rt.failure.empty() ? std::string("ONNX Runtime is not usable") : rt.failure);
        return engine;
    }
    const OrtApi* api = rt.api;
    engine->m_api = api;

    OrtSessionOptions* opts = nullptr;
    std::string err = takeStatus(api, api->CreateSessionOptions(&opts));
    if (!err.empty() || opts == nullptr) {
        engine->setDetail("session options: " + err);
        return engine;
    }
    // Release the options on every path out of here.
    std::unique_ptr<OrtSessionOptions, void (*)(OrtSessionOptions*)> optsGuard(opts, [](OrtSessionOptions* o) {
        if (const OrtRuntime& r = ortRuntime(); r.api != nullptr) {
            r.api->ReleaseSessionOptions(o);
        }
    });

    // The work is on the GPU; ORT's own CPU thread pool only runs shape
    // arithmetic.  One thread, not spinning, so a host process does not find
    // a core per CPU busy-waiting on its behalf.
    err = takeStatus(api, api->SetIntraOpNumThreads(opts, 1));
    if (err.empty()) {
        err = takeStatus(api, api->SetInterOpNumThreads(opts, 1));
    }
    if (err.empty()) {
        err = takeStatus(api, api->AddSessionConfigEntry(opts, "session.intra_op.allow_spinning", "0"));
    }
    if (err.empty()) {
        err = takeStatus(api, api->SetSessionGraphOptimizationLevel(opts, ORT_ENABLE_ALL));
    }
    if (!err.empty()) {
        engine->setDetail("session options: " + err);
        return engine;
    }

    // CUDA execution provider.
    //   cudnn_conv_algo_search=HEURISTIC  measured on the 2048 x 68 band:
    //       HEURISTIC 175 ms, EXHAUSTIVE 161 ms (within run-to-run noise, and
    //       it benchmarks every algorithm again for each new input shape),
    //       DEFAULT 980 ms.  So the explicit setting matters, and the cheap
    //       one is as good as the thorough one here.
    //   arena_extend_strategy=kSameAsRequested  grow the GPU arena by what
    //       is asked for, not by powers of two, so a host keeps the memory
    //       it does not need.
    OrtCUDAProviderOptionsV2* cuda = nullptr;
    err = takeStatus(api, api->CreateCUDAProviderOptions(&cuda));
    if (err.empty() && cuda != nullptr) {
        const std::array<const char*, 4> keys = {"device_id", "cudnn_conv_algo_search", "arena_extend_strategy",
                                                 "do_copy_in_default_stream"};
        const std::array<const char*, 4> values = {"0", "HEURISTIC", "kSameAsRequested", "1"};
        err = takeStatus(api, api->UpdateCUDAProviderOptions(cuda, keys.data(), values.data(), keys.size()));
        if (err.empty()) {
            QuietDllErrors quiet;
            err = takeStatus(api, api->SessionOptionsAppendExecutionProvider_CUDA_V2(opts, cuda));
        }
        api->ReleaseCUDAProviderOptions(cuda);
    }
    if (!err.empty()) {
        // The provider's own message names a DLL but is often misleading
        // (it blames cudnn64_9.dll for a missing cudart); the dependency
        // diagnosis says what was actually searched.
        std::string why = "CUDA execution provider unavailable: " + err;
        if (!rt.cudaProblems.empty()) {
            why += " [dependencies: " + rt.cudaProblems + "]";
        }
        why += " - CPU inference is not used, see FlowBackendOnnx.cpp";
        engine->setDetail(std::move(why));
        return engine;
    }

    // Load the model.  This is where a missing GPU, a driver too old for the
    // CUDA runtime, or a corrupt file surfaces.
    const auto t0 = std::chrono::steady_clock::now();
    {
        QuietDllErrors quiet;
        err = takeStatus(api, api->CreateSession(rt.env, model.c_str(), opts, &engine->m_session));
    }
    if (!err.empty() || engine->m_session == nullptr) {
        engine->m_session = nullptr;
        std::string why = "could not create a session for " + utf8(model) + ": " + err;
        if (!rt.cudaProblems.empty()) {
            why += " [dependencies: " + rt.cudaProblems + "]";
        }
        engine->setDetail(std::move(why));
        return engine;
    }

    if (std::string bad = engine->validateSignature(); !bad.empty()) {
        api->ReleaseSession(engine->m_session);
        engine->m_session = nullptr;
        engine->setDetail(utf8(model.filename()) + " is not a SEA-RAFT flow model this build understands: " + bad);
        return engine;
    }

    err = takeStatus(api, api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &engine->m_cpuInfo));
    if (!err.empty() || engine->m_cpuInfo == nullptr) {
        api->ReleaseSession(engine->m_session);
        engine->m_session = nullptr;
        engine->setDetail("CPU memory info: " + err);
        return engine;
    }

    // Self-test: one real inference, both directions, at the smallest shape
    // the graph accepts, BEFORE the engine is allowed to call itself usable.
    //
    // Session creation alone proves too little.  ONNX Runtime 1.30 loads
    // cuDNN lazily, at the first convolution, so with cudnn64_9.dll missing
    // CreateSession SUCCEEDS and every inference then fails - measured, by
    // hiding the DLL.  Without this check the backend would report itself
    // available and fail per frame, breaking FlowBackend.h's promise that an
    // unusable backend says so before compute() is called.  It also pays the
    // one-time CUDA/cuDNN initialisation here instead of on the first frame.
    {
        constexpr std::int64_t n = 2;
        constexpr std::int64_t side = kMinModelEdge;
        std::vector<float> probeA(static_cast<std::size_t>(n * side * side));
        std::vector<float> probeB(probeA.size());
        for (std::size_t i = 0; i < probeA.size(); ++i) {
            // Any texture will do; this one is cheap and not constant.
            const auto x = static_cast<float>(i % static_cast<std::size_t>(side));
            const auto y = static_cast<float>((i / static_cast<std::size_t>(side)) % static_cast<std::size_t>(side));
            probeA[i] = 0.5f + 0.25f * std::sin(0.37f * x) * std::cos(0.23f * y);
            probeB[i] = 0.5f + 0.25f * std::sin(0.37f * (x - 2.0f)) * std::cos(0.23f * y);
        }
        std::vector<float> probeOut;
        const Status probe = engine->run(probeA, probeB, n, side, side, probeOut);
        const bool finite =
            probe.ok() && std::all_of(probeOut.begin(), probeOut.end(), [](float v) { return std::isfinite(v); });
        if (!finite) {
            std::string why = "the self-test inference failed: " +
                              (probe.ok() ? std::string("non-finite output") : probe.error().message);
            if (!rt.cudaProblems.empty()) {
                why += " [dependencies: " + rt.cudaProblems + "]";
            }
            api->ReleaseSession(engine->m_session);
            engine->m_session = nullptr;
            engine->setDetail(std::move(why));
            return engine;
        }
    }

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    engine->m_healthyDetail = "SEA-RAFT (" + utf8(model.filename()) + ") on CUDA device 0, ONNX Runtime " + rt.version;
    engine->setDetail(engine->m_healthyDetail);
    log::info("neural flow: {} ready in {:.0f} ms", log::safe(engine->m_healthyDetail), ms);
    return engine;
}

Status OrtEngine::run(const std::vector<float>& a, const std::vector<float>& b, std::int64_t n, std::int64_t h,
                      std::int64_t w, std::vector<float>& flow) {
    if (!usable()) {
        return Error{ErrorCode::Unsupported, detail()};
    }
    const std::size_t plane = static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
    const std::size_t inCount = static_cast<std::size_t>(n) * plane;
    if (n <= 0 || h <= 0 || w <= 0 || a.size() != inCount || b.size() != inCount) {
        return Error{ErrorCode::InvalidArgument, "OrtEngine::run: tensor sizes do not match the shape"};
    }

    std::lock_guard<std::mutex> lock(m_runMutex);
    const OrtApi* api = m_api;
    const std::array<std::int64_t, 4> shape = {n, 1, h, w};
    ValueGuard va(api);
    ValueGuard vb(api);
    ValueGuard out(api);
    // ORT takes a non-const pointer but does not write to inputs.
    std::string err = takeStatus(api, api->CreateTensorWithDataAsOrtValue(
                                          m_cpuInfo, const_cast<float*>(a.data()), inCount * sizeof(float),
                                          shape.data(), shape.size(), ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, va.put()));
    if (err.empty()) {
        err = takeStatus(api, api->CreateTensorWithDataAsOrtValue(m_cpuInfo, const_cast<float*>(b.data()),
                                                                  inCount * sizeof(float), shape.data(), shape.size(),
                                                                  ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, vb.put()));
    }
    if (err.empty()) {
        const std::array<const char*, 2> inNames = {"image_a", "image_b"};
        const std::array<const OrtValue*, 2> inValues = {va.get(), vb.get()};
        const std::array<const char*, 1> outNames = {"flow"};
        err = takeStatus(api, api->Run(m_session, nullptr, inNames.data(), inValues.data(), inValues.size(),
                                       outNames.data(), outNames.size(), out.put()));
    }

    // Validate the output's shape before trusting a single float of it.
    if (err.empty() && out.get() != nullptr) {
        OrtTensorTypeAndShapeInfo* info = nullptr;
        err = takeStatus(api, api->GetTensorTypeAndShape(out.get(), &info));
        std::size_t rank = 0;
        std::array<std::int64_t, 4> dims{};
        if (err.empty() && info != nullptr) {
            err = takeStatus(api, api->GetDimensionsCount(info, &rank));
            if (err.empty() && rank == 4) {
                err = takeStatus(api, api->GetDimensions(info, dims.data(), dims.size()));
            }
        }
        if (info != nullptr) {
            api->ReleaseTensorTypeAndShapeInfo(info);
        }
        if (err.empty() && (rank != 4 || dims[0] != n || dims[1] != 2 || dims[2] != h || dims[3] != w)) {
            err = "the model returned a tensor of the wrong shape";
        }
    } else if (err.empty()) {
        err = "the model returned no output";
    }

    float* data = nullptr;
    if (err.empty()) {
        err = takeStatus(api, api->GetTensorMutableData(out.get(), reinterpret_cast<void**>(&data)));
        if (err.empty() && data == nullptr) {
            err = "the model output has no data";
        }
    }
    if (!err.empty()) {
        const int failures = ++m_consecutiveFailures;
        if (failures >= kMaxConsecutiveFailures) {
            setDetail("disabled after " + std::to_string(failures) + " consecutive inference failures; last: " + err);
        }
        return Error{ErrorCode::Gpu, "neural flow inference failed: " + err};
    }

    flow.assign(data, data + static_cast<std::size_t>(n) * 2 * plane);
    if (m_consecutiveFailures.exchange(0) != 0) {
        setDetail(m_healthyDetail);
    }
    return okStatus();
}

// ===========================================================================
//  The engine cache.
// ===========================================================================

/// Engines keyed by model path.  An entry is reused while the file's size and
/// timestamp are unchanged - including a FAILED entry, so a missing model
/// costs one stat() per frame rather than a session attempt - and rebuilt as
/// soon as they change, so installing the model needs no restart.
class EngineCache {
public:
    std::shared_ptr<OrtEngine> get(const fs::path& model) {
        const Stamp stamp = stampOf(model);
        const std::wstring key = model.wstring();
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_entries.find(key);
        if (it != m_entries.end() && it->second.stamp == stamp && it->second.engine) {
            return it->second.engine;
        }
        // Built under the lock on purpose: a second thread asking for the
        // same model waits for this one rather than loading it twice.
        std::shared_ptr<OrtEngine> engine = OrtEngine::create(model);
        m_entries[key] = Entry{stamp, engine};
        return engine;
    }

private:
    struct Stamp {
        bool exists = false;
        std::uintmax_t size = 0;
        fs::file_time_type time{};
        bool operator==(const Stamp&) const = default;
    };
    struct Entry {
        Stamp stamp;
        std::shared_ptr<OrtEngine> engine;
    };

    static Stamp stampOf(const fs::path& p) {
        Stamp s;
        std::error_code ec;
        if (p.empty() || !fs::is_regular_file(p, ec)) {
            return s;
        }
        s.exists = true;
        s.size = fs::file_size(p, ec);
        s.time = fs::last_write_time(p, ec);
        return s;
    }

    std::mutex m_mutex;
    std::map<std::wstring, Entry> m_entries;
};

EngineCache& engineCache() {
    static EngineCache* cache = new EngineCache();  // intentionally leaked, see LIFETIME
    return *cache;
}

/// The model file a set of parameters refers to.
fs::path resolveModelPath(const FlowBackendParams& params) {
    if (!params.modelPath.empty()) {
        return pathFromUtf8(params.modelPath);
    }
    const fs::path dir = thisModuleDir();
    return dir.empty() ? fs::path() : dir / L"models" / kDefaultModelFile;
}

// ===========================================================================
//  Geometry: working resolution and tiles.
// ===========================================================================

/// Round up to a multiple of kModelAlign.
std::uint32_t alignUp(double v) {
    const double units = std::ceil(v / static_cast<double>(kModelAlign) - 1e-9);
    return static_cast<std::uint32_t>(std::max(1.0, units)) * kModelAlign;
}

/// Tile origins along one axis: uniform tiles of `tile` covering [0, total),
/// stepping by tile - overlap, with the last tile pushed flush to the end so
/// every tile has the same size (one cuDNN tuning per band, not two).
std::vector<std::uint32_t> tileOrigins(std::uint32_t total, std::uint32_t tile, std::uint32_t overlap) {
    std::vector<std::uint32_t> xs;
    if (tile >= total) {
        xs.push_back(0);
        return xs;
    }
    const std::uint32_t step = std::max<std::uint32_t>(kModelAlign, tile - std::min(overlap, tile / 2));
    for (std::uint32_t x = 0;; x += step) {
        if (x + tile >= total) {
            xs.push_back(total - tile);
            break;
        }
        xs.push_back(x);
    }
    // The flush-right tile can coincide with the previous one on exact fits.
    xs.erase(std::unique(xs.begin(), xs.end()), xs.end());
    return xs;
}

/// Everything about how one compute() call maps onto the network.
struct Geometry {
    std::uint32_t w = 0, h = 0;          ///< caller's size
    std::uint32_t W = 0, H = 0;          ///< working size (multiples of 8, >= 128)
    std::uint32_t tileW = 0, tileH = 0;  ///< uniform tile size
    std::vector<std::uint32_t> xs, ys;   ///< tile origins
};

Geometry planGeometry(std::uint32_t w, std::uint32_t h, const FlowBackendParams& params) {
    Geometry g;
    g.w = w;
    g.h = h;

    // Isotropic upscale so the band is tall enough (and, for a pathological
    // narrow input, wide enough); never downscale - resolution is accuracy.
    const double s = std::max({1.0, static_cast<double>(kMinWorkingRows) / static_cast<double>(h),
                               static_cast<double>(kMinModelEdge) / static_cast<double>(w)});
    g.W = std::max(kMinModelEdge, alignUp(static_cast<double>(w) * s));
    g.H = std::max(kMinModelEdge, alignUp(static_cast<double>(h) * s));

    // Tile edge from the parameter, clamped to what the graph can run and
    // aligned; then shrink the tile HEIGHT first (a band is wide, so height
    // is the cheap axis to split) until the area cap holds.
    std::uint32_t edge =
        static_cast<std::uint32_t>(std::clamp<std::int64_t>(params.maxTileEdge, kMinModelEdge, kMaxTileEdgeCeiling));
    edge = std::max(kMinModelEdge, (edge / kModelAlign) * kModelAlign);
    g.tileW = std::min(g.W, edge);
    g.tileH = std::min(g.H, edge);
    // Straight to the largest aligned size that fits, rather than halving:
    // halving took a 768-row strip from 768 to 384 to 192 rows and so cut it
    // into six tile rows where three and a bit of 256 fit the same cap.
    const auto fit = [](std::uint32_t current, std::uint32_t other) {
        const std::uint64_t most = (kMaxTilePixels / std::max<std::uint32_t>(other, 1u) / kModelAlign) * kModelAlign;
        return static_cast<std::uint32_t>(
            std::clamp<std::uint64_t>(most, kMinModelEdge, std::max<std::uint32_t>(current, kMinModelEdge)));
    };
    if (static_cast<std::uint64_t>(g.tileW) * g.tileH > kMaxTilePixels) {
        g.tileH = std::min(g.tileH, fit(g.tileH, g.tileW));
    }
    if (static_cast<std::uint64_t>(g.tileW) * g.tileH > kMaxTilePixels) {
        g.tileW = std::min(g.tileW, fit(g.tileW, g.tileH));
    }

    const std::uint32_t overlap = static_cast<std::uint32_t>(std::clamp(params.tileOverlapPx, 0, 1 << 15));
    g.xs = tileOrigins(g.W, g.tileW, overlap);
    g.ys = tileOrigins(g.H, g.tileH, overlap);
    return g;
}

/// Cross-fade weights for one tile along one axis.
///
/// Raised cosine across the FULL overlap with each neighbour, so at every
/// working pixel the two tiles' weights sum to one and each tile's weight
/// reaches zero exactly at its own edge - the edge being where its flow is
/// least trustworthy, because the network could not see past it.  An image
/// border is not a tile edge and is not faded.
std::vector<float> axisWeights(const std::vector<std::uint32_t>& origins, std::size_t index, std::uint32_t tile) {
    std::vector<float> wts(tile, 1.0f);
    const std::uint32_t x0 = origins[index];
    const std::uint32_t x1 = x0 + tile;
    const auto ramp = [](std::uint32_t i, std::uint32_t len) {
        // i in [0, len): rises from ~0 to ~1; samples at pixel centres so
        // neither end is exactly 0, which keeps the weight sum positive.
        const double t = (static_cast<double>(i) + 0.5) / static_cast<double>(len);
        return static_cast<float>(0.5 - 0.5 * std::cos(3.14159265358979323846 * t));
    };
    if (index > 0) {
        const std::uint32_t prevEnd = origins[index - 1] + tile;
        const std::uint32_t len = prevEnd > x0 ? std::min(prevEnd - x0, tile) : 0u;
        for (std::uint32_t i = 0; i < len; ++i) {
            wts[i] *= ramp(i, len);
        }
    }
    if (index + 1 < origins.size()) {
        const std::uint32_t nextStart = origins[index + 1];
        const std::uint32_t len = x1 > nextStart ? std::min(x1 - nextStart, tile) : 0u;
        for (std::uint32_t i = 0; i < len; ++i) {
            wts[tile - 1 - i] *= ramp(i, len);
        }
    }
    return wts;
}

// ===========================================================================
//  Resampling.
// ===========================================================================

/// Run `body(row)` over rows, on the pool when there is one.
Status forRows(ThreadPool* pool, std::size_t rows, const std::function<void(std::size_t)>& body) {
    if (pool != nullptr && rows > 1) {
        return pool->parallelRows(rows, 8, body);
    }
    for (std::size_t r = 0; r < rows; ++r) {
        body(r);
    }
    return okStatus();
}

/// A value the network can be fed: finite and inside [0, 1].  Band luma is
/// code-space in [0, 1] already; this guards against a NaN from upstream
/// and against out-of-range values the network never saw in training.
inline float sanitizeInput(float v) {
    return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 0.0f;
}

/// Bilinear resample with pixel-centre alignment and clamp-to-edge:
/// dst(x, y) = src((x + 0.5) * sw / dw - 0.5, ...) scaled by `gain`.
/// `sanitize` applies sanitizeInput to every source tap.
Status resample(const float* src, std::uint32_t sw, std::uint32_t sh, float* dst, std::uint32_t dw, std::uint32_t dh,
                float gain, bool sanitize, ThreadPool* pool) {
    if (src == nullptr || dst == nullptr || sw == 0 || sh == 0 || dw == 0 || dh == 0) {
        return Error{ErrorCode::InvalidArgument, "resample: empty plane"};
    }
    const double fx = static_cast<double>(sw) / static_cast<double>(dw);
    const double fy = static_cast<double>(sh) / static_cast<double>(dh);
    // Column taps are the same for every row: precompute them once.
    std::vector<std::uint32_t> x0s(dw), x1s(dw);
    std::vector<float> wxs(dw);
    for (std::uint32_t x = 0; x < dw; ++x) {
        const double sx = std::clamp((static_cast<double>(x) + 0.5) * fx - 0.5, 0.0, static_cast<double>(sw - 1));
        const auto i0 = static_cast<std::uint32_t>(sx);
        x0s[x] = i0;
        x1s[x] = std::min(i0 + 1, sw - 1);
        wxs[x] = static_cast<float>(sx - static_cast<double>(i0));
    }
    return forRows(pool, dh, [&](std::size_t y) {
        const double sy = std::clamp((static_cast<double>(y) + 0.5) * fy - 0.5, 0.0, static_cast<double>(sh - 1));
        const auto j0 = static_cast<std::uint32_t>(sy);
        const std::uint32_t j1 = std::min(j0 + 1, sh - 1);
        const float wy = static_cast<float>(sy - static_cast<double>(j0));
        const float* r0 = src + static_cast<std::size_t>(j0) * sw;
        const float* r1 = src + static_cast<std::size_t>(j1) * sw;
        float* out = dst + y * dw;
        for (std::uint32_t x = 0; x < dw; ++x) {
            float a = r0[x0s[x]], b = r0[x1s[x]], c = r1[x0s[x]], d = r1[x1s[x]];
            if (sanitize) {
                a = sanitizeInput(a);
                b = sanitizeInput(b);
                c = sanitizeInput(c);
                d = sanitizeInput(d);
            }
            const float top = a + (b - a) * wxs[x];
            const float bot = c + (d - c) * wxs[x];
            out[x] = (top + (bot - top) * wy) * gain;
        }
    });
}

// ===========================================================================
//  The backend.
// ===========================================================================

class OnnxFlowBackend final : public FlowBackend {
public:
    explicit OnnxFlowBackend(std::shared_ptr<OrtEngine> engine)
        : m_engine(std::move(engine)) {}

    [[nodiscard]] FlowBackendInfo info() const override {
        FlowBackendInfo out;
        out.kind = FlowBackendKind::Neural;
        out.available = isAvailable();
        out.detail = m_engine ? m_engine->detail() : std::string("no engine");
        // cuDNN may pick different convolution algorithms run to run, and
        // some are not bit-reproducible; say so rather than promise it.
        out.deterministic = false;
        return out;
    }

    [[nodiscard]] bool isAvailable() const override { return m_engine && m_engine->usable(); }

    [[nodiscard]] Result<BidirFlow> compute(const GrayImage& a, const GrayImage& b, const FlowBackendParams& params,
                                            ThreadPool* pool) override {
        // Nothing may escape into a host: allocation failure on a very large
        // band is the one realistic throw left once inputs are validated.
        try {
            return computeImpl(a, b, params, pool);
        } catch (const std::bad_alloc&) {
            return Error{ErrorCode::Internal, "neural flow: out of memory"};
        } catch (const std::exception& e) {
            return Error{ErrorCode::Internal, std::string("neural flow: ") + e.what()};
        } catch (...) {
            return Error{ErrorCode::Internal, "neural flow: unknown exception"};
        }
    }

private:
    Result<BidirFlow> computeImpl(const GrayImage& a, const GrayImage& b, const FlowBackendParams& params,
                                  ThreadPool* pool);

    std::shared_ptr<OrtEngine> m_engine;
};

Result<BidirFlow> OnnxFlowBackend::computeImpl(const GrayImage& a, const GrayImage& b, const FlowBackendParams& params,
                                               ThreadPool* pool) {
    if (!isAvailable()) {
        return Error{ErrorCode::Unsupported, m_engine ? m_engine->detail() : std::string("no engine")};
    }
    if (!a.valid() || !b.valid()) {
        return Error{ErrorCode::InvalidArgument, "neural flow: empty or malformed image"};
    }
    if (a.w != b.w || a.h != b.h) {
        return Error{ErrorCode::InvalidArgument, "neural flow: the two images differ in size"};
    }
    if (a.w > kMaxInputEdge || a.h > kMaxInputEdge) {
        return Error{ErrorCode::InvalidArgument, "neural flow: image larger than the backend accepts"};
    }

    const auto t0 = std::chrono::steady_clock::now();
    const Geometry g = planGeometry(a.w, a.h, params);
    const std::size_t workN = static_cast<std::size_t>(g.W) * g.H;

    // ---- 1. Both images at working resolution, sanitised to [0, 1]. ----
    std::vector<float> A(workN), B(workN);
    OSV_TRY(resample(a.data.data(), a.w, a.h, A.data(), g.W, g.H, 1.0f, true, pool));
    OSV_TRY(resample(b.data.data(), b.w, b.h, B.data(), g.W, g.H, 1.0f, true, pool));

    // ---- 2. Tiles, both directions per batch, cross-faded together. ----
    // Accumulators: forward u, v; backward u, v; weight.
    std::vector<float> fu(workN, 0.0f), fv(workN, 0.0f), bu(workN, 0.0f), bv(workN, 0.0f), wsum(workN, 0.0f);
    const std::size_t tilePlane = static_cast<std::size_t>(g.tileW) * g.tileH;
    std::vector<float> inA(2 * tilePlane), inB(2 * tilePlane), out;
    for (std::size_t ty = 0; ty < g.ys.size(); ++ty) {
        const std::vector<float> wy = axisWeights(g.ys, ty, g.tileH);
        for (std::size_t tx = 0; tx < g.xs.size(); ++tx) {
            const std::vector<float> wx = axisWeights(g.xs, tx, g.tileW);
            const std::uint32_t x0 = g.xs[tx];
            const std::uint32_t y0 = g.ys[ty];

            // Batch element 0 is (A, B) - forward; element 1 is (B, A) -
            // backward.  image_a gets [A; B] and image_b gets [B; A].
            for (std::uint32_t r = 0; r < g.tileH; ++r) {
                const std::size_t src = static_cast<std::size_t>(y0 + r) * g.W + x0;
                const std::size_t dst = static_cast<std::size_t>(r) * g.tileW;
                std::copy_n(A.data() + src, g.tileW, inA.data() + dst);
                std::copy_n(B.data() + src, g.tileW, inA.data() + tilePlane + dst);
                std::copy_n(B.data() + src, g.tileW, inB.data() + dst);
                std::copy_n(A.data() + src, g.tileW, inB.data() + tilePlane + dst);
            }
            OSV_TRY(m_engine->run(inA, inB, 2, g.tileH, g.tileW, out));

            // out layout [2 batch][2 channel][tileH][tileW].
            const float* ofu = out.data();
            const float* ofv = out.data() + tilePlane;
            const float* obu = out.data() + 2 * tilePlane;
            const float* obv = out.data() + 3 * tilePlane;
            for (std::uint32_t r = 0; r < g.tileH; ++r) {
                const std::size_t row = static_cast<std::size_t>(y0 + r) * g.W + x0;
                const std::size_t t = static_cast<std::size_t>(r) * g.tileW;
                for (std::uint32_t c = 0; c < g.tileW; ++c) {
                    const float wgt = wy[r] * wx[c];
                    fu[row + c] += wgt * ofu[t + c];
                    fv[row + c] += wgt * ofv[t + c];
                    bu[row + c] += wgt * obu[t + c];
                    bv[row + c] += wgt * obv[t + c];
                    wsum[row + c] += wgt;
                }
            }
        }
    }
    for (std::size_t i = 0; i < workN; ++i) {
        // Every working pixel lies inside at least one tile at a strictly
        // positive weight (see axisWeights), so this never divides by zero;
        // the guard is for a future edit that breaks that.
        const float inv = wsum[i] > 0.0f ? 1.0f / wsum[i] : 0.0f;
        fu[i] *= inv;
        fv[i] *= inv;
        bu[i] *= inv;
        bv[i] *= inv;
    }

    // ---- 3. Back to the caller's resolution, vectors rescaled per axis. ----
    BidirFlow result;
    result.forward.resize(a.w, a.h);
    result.backward.resize(a.w, a.h);
    const float gx = static_cast<float>(static_cast<double>(a.w) / static_cast<double>(g.W));
    const float gy = static_cast<float>(static_cast<double>(a.h) / static_cast<double>(g.H));
    OSV_TRY(resample(fu.data(), g.W, g.H, result.forward.u.data(), a.w, a.h, gx, false, pool));
    OSV_TRY(resample(fv.data(), g.W, g.H, result.forward.v.data(), a.w, a.h, gy, false, pool));
    OSV_TRY(resample(bu.data(), g.W, g.H, result.backward.u.data(), a.w, a.h, gx, false, pool));
    OSV_TRY(resample(bv.data(), g.W, g.H, result.backward.v.data(), a.w, a.h, gy, false, pool));

    // ---- 4. Forward-backward consistency, as disFlowBidirectional does it.
    // A non-finite vector (never observed, but a GPU can misbehave) is
    // zeroed - no NaN may leave this function - and marked inconsistent.
    std::uint64_t nonFinite = 0;
    for (FlowField* f : {&result.forward, &result.backward}) {
        for (std::size_t i = 0; i < f->u.size(); ++i) {
            if (!std::isfinite(f->u[i]) || !std::isfinite(f->v[i])) {
                f->u[i] = 0.0f;
                f->v[i] = 0.0f;
                ++nonFinite;
            }
        }
    }
    if (nonFinite == static_cast<std::uint64_t>(result.forward.u.size()) * 2u) {
        return Error{ErrorCode::Gpu, "neural flow: the network produced no finite vectors"};
    }
    if (nonFinite > 0) {
        log::warn("neural flow: {} non-finite vectors zeroed", nonFinite);
    }

    // Identical semantics to disFlowBidirectional (DisFlow.cpp): follow the
    // forward vector to the nearest pixel, read the backward vector there
    // (clamp-to-edge), and accept when their sum is within the tolerance.
    result.ok.assign(result.forward.u.size(), 0u);
    const int w = static_cast<int>(a.w);
    const int h = static_cast<int>(a.h);
    const double tol = std::max(0.0, params.consistencyTolPx);
    const double tol2 = tol * tol;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * a.w + static_cast<std::size_t>(x);
            const float ffu = result.forward.u[idx];
            const float ffv = result.forward.v[idx];
            const int qx = static_cast<int>(std::lround(static_cast<double>(x) + ffu));
            const int qy = static_cast<int>(std::lround(static_cast<double>(y) + ffv));
            const double ex = static_cast<double>(ffu) + result.backward.atU(qx, qy);
            const double ey = static_cast<double>(ffv) + result.backward.atV(qx, qy);
            if (ex * ex + ey * ey <= tol2) {
                result.ok[idx] = 1u;
                ++result.consistent;
            }
        }
    }

    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    log::debug("neural flow: {}x{} via {}x{} in {}x{} tiles of {}x{}: {:.1f} ms, {:.1f}% consistent", a.w, a.h, g.W,
               g.H, g.xs.size(), g.ys.size(), g.tileW, g.tileH, ms,
               100.0 * static_cast<double>(result.consistent) / static_cast<double>(result.ok.size()));
    return result;
}

}  // namespace

std::unique_ptr<FlowBackend> makeOnnxFlowBackend(const FlowBackendParams& params) {
    // Never throws: the factory promises the caller a backend or null, and
    // FlowBackend.cpp turns null into an "unavailable" with a reason.
    try {
        std::shared_ptr<OrtEngine> engine = engineCache().get(resolveModelPath(params));
        return std::make_unique<OnnxFlowBackend>(std::move(engine));
    } catch (...) {
        return nullptr;
    }
}

}  // namespace osv::render

#endif  // OSV_HAVE_ONNXRUNTIME
