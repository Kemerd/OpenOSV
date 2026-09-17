// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MockHost: a fake Premiere Pro host for unit tests.
//
// A plug-in only ever talks to Premiere through two things: the piSuites
// callback table (legacy memory functions and getSPBasicSuite) and the
// SweetPea suites it acquires by (name, version).  MockHost owns both and
// serves this repository's own implementations of every suite the importer
// and the effect use:
//
//   Premiere PPix Suite v1, PPix 2 Suite v3, PPix Creator v1,
//   PPix Creator 2 v4, PPix Cache v8 and v7 (a real small LRU),
//   Time v1, MediaCore StringSuite v1, App Info v3, Error v3,
//   Color Management v1, Memory Manager v4, Importer File Manager v4,
//   Sequence Info v9, Video Segment v9/v8/v7/v6 (keyframe table with
//   interpolation), GPU Device v2 (a real CUDA driver-API context when the
//   build has CUDA and a device is present), PF Pixel Format v1 and
//   PF Utility v4 (AE side) plus PF_InData / PF_OutData builders with
//   working checkout_param / add_param callbacks.
//
// Everything is inspectable: created PPixes, cache hits, live strings,
// acquire/release reference counts, error events, and every suite can be
// hidden with setSuiteAvailable() to simulate an older host.
//
// Only one MockHost may exist at a time (the suite functions are plain C
// function pointers and reach the instance through MockHost::current()).
// All methods are thread-safe.
//
// Row convention of host PPixes created here: positive row bytes rounded
// up to 128, but the first row in memory is the BOTTOM scanline, exactly
// like Premiere's uncompressed 4444 formats.  GPU PPixes are top-left with a
// 256-byte aligned pitch.  Effect worlds are top-left like After Effects.
#pragma once

#include "PrSDKTypes.h"
#include "PrSDKPlugSuites.h"
#include "SPBasic.h"

#include "PrSDKColorSEICodes.h"
#include "PrSDKImmersiveVideoTypes.h"
#include "PrSDKPixelFormat.h"
#include "PrSDKTimeSuite.h"

#include "AE_Effect.h"
#include "PrSDKAESupport.h"

// The custom-UI and DrawBot surface (MockDrawbot.cpp).  AE_EffectUI.h brings
// PF_ContextH / PF_WindowType and adobesdk/DrawbotSuite.h the DRAWBOT_* types
// the records below are built from.
#include "AE_EffectUI.h"

#include "adobesdk/DrawbotSuite.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osv::premiere::mock {

/// Ticks per second the mock Time Suite reports (matches Premiere).
constexpr PrTime kTicksPerSecond = 254016000000ll;

/// What the inspector reports about a PPix.
struct PPixInfo {
    void* pixels = nullptr;          ///< Host address of row 0 (bottom scanline), or the device pointer for GPU PPixes.
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::int32_t rowBytes = 0;
    PrPixelFormat format = PrPixelFormat_Invalid;
    PrSDKColorSpaceID colorSpace{};  ///< kPrSDKColorSpaceID_Invalid unless created colour managed.
    bool isGpu = false;
    csSDK_uint32 deviceIndex = 0;
    std::size_t byteSize = 0;        ///< Allocated bytes (rowBytes * height).
    int refCount = 0;                ///< Outstanding references (creator + cache + clones).
    csSDK_uint32 parNum = 1;
    csSDK_uint32 parDen = 1;
    prFieldType fieldType = prFieldsNone;
};

/// One event reported through the Error Suite.
struct ErrorEvent {
    csSDK_uint32 type = 0;  ///< kEventTypeInformational / Warning / Error plus flags.
    std::wstring title;
    std::wstring description;
};

/// Per-timeline configuration served by the Sequence Info Suite.
struct SequenceConfig {
    prRect frameRect{0, 0, 1920, 1080};
    csSDK_uint32 parNum = 1;
    csSDK_uint32 parDen = 1;
    PrTime ticksPerFrame = kTicksPerSecond / 30;
    prFieldType fieldType = prFieldsNone;
    PrTime zeroPoint = 0;
    bool dropFrame = false;
    bool proxy = false;
    PrIVProjectionType projection = kPrIVProjectionType_None;
    PrIVFrameLayout layout = kPrIVFrameLayout_Monoscopic;
    csSDK_uint32 horizontalView = 360;
    csSDK_uint32 verticalView = 180;
    PrSDKColorSpaceID workingColorSpace{};  ///< Defaults to the id of kPrRec709.
    csSDK_uint32 graphicsWhiteLuminance = 203;
    csSDK_uint32 lutInterpolation = 0;
    bool autoToneMap = false;
    csSDK_uint32 sdrGamma = 0;
};

/// Application identity served by the App Info Suite.
struct AppIdentity {
    csSDK_uint32 fourcc = 'PPro';
    unsigned major = 26;
    unsigned minor = 2;
    unsigned patch = 2;
    csSDK_uint32 build = 12;
    std::string language = "en_US";
};

// ---------------------------------------------------------------------------
//  The DrawBot recording
//
//  The mock's DrawBot suites do not rasterise anything; they write down what
//  they were asked to draw, in order, and a test reads the list back.  These
//  are the record types.  They are declared here, after every Adobe include
//  has popped its #pragma pack, so they keep natural alignment.
// ---------------------------------------------------------------------------

/// What a recorded path vertex is.
enum class DrawbotVertexKind : int {
    MoveTo = 0,
    LineTo = 1,
    BezierTo = 2,  ///< Only the end point is recorded.
    Rect = 3,      ///< x/y = origin, a/b = width/height.
    Arc = 4,       ///< x/y = centre, a = radius, b = start angle, c = sweep.
    Close = 5,
};

/// One vertex of a recorded path.
///
/// One struct for every kind rather than a variant: a test reads two or
/// three fields and a flat POD is far easier to assert against than a
/// visitor.  Which fields carry meaning is documented per kind above.
struct DrawbotVertex {
    DrawbotVertexKind kind = DrawbotVertexKind::MoveTo;
    float x = 0.0f;
    float y = 0.0f;
    float a = 0.0f;  ///< Rect width, or arc radius.
    float b = 0.0f;  ///< Rect height, or arc start angle in degrees.
    float c = 0.0f;  ///< Arc sweep in degrees.
};

/// A path created through the Supplier Suite.
struct DrawbotPathRecord {
    std::vector<DrawbotVertex> vertices;
    bool live = false;  ///< False once ReleaseObject was called on it.
};

/// A pen created through the Supplier Suite.
struct DrawbotPenRecord {
    DRAWBOT_ColorRGBA colour{};
    float width = 0.0f;
    int dashSegments = 0;
    bool live = false;
};

/// A brush created through the Supplier Suite.
struct DrawbotBrushRecord {
    DRAWBOT_ColorRGBA colour{};
    bool live = false;
};

/// A font created through the Supplier Suite.
struct DrawbotFontRecord {
    float size = 0.0f;
    bool live = false;
};

/// What a recorded drawing operation is.
enum class DrawbotOpKind : int {
    StrokePath = 0,
    FillPath = 1,
};

/// One stroke or fill.
///
/// `dx`/`dy` carry the surface translation in force at the time, which is
/// how a test separates the overlay's dark 1px shadow pass (offset by one)
/// from the light ink pass (no offset).
struct DrawbotDrawOp {
    DrawbotOpKind kind = DrawbotOpKind::StrokePath;
    std::size_t pathIndex = static_cast<std::size_t>(-1);  ///< Index into DrawbotRecord::paths.
    DRAWBOT_ColorRGBA colour{};
    float width = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    bool staleObject = false;  ///< The pen/brush had already been released.
};

/// One string drawn, decoded to UTF-8.
struct DrawbotStringOp {
    std::string text;
    float x = 0.0f;
    float y = 0.0f;
    float fontSize = 0.0f;
    DRAWBOT_ColorRGBA colour{};
};

/// One PaintRect call.  The overlay must never make any: a filled rectangle
/// over the picture is exactly what a non-intrusive HUD is not.
struct DrawbotPaintRect {
    DRAWBOT_ColorRGBA colour{};
    DRAWBOT_RectF32 rect{};
};

/// Everything one draw pass did, as a test reads it.
struct DrawbotRecord {
    std::vector<DrawbotPathRecord> paths;
    std::vector<DrawbotDrawOp> ops;
    std::vector<DrawbotStringOp> strings;
    std::vector<DrawbotPaintRect> paintRects;
    std::vector<DrawbotPenRecord> pens;
    std::vector<DrawbotBrushRecord> brushes;

    std::size_t retainCount = 0;
    std::size_t releaseCount = 0;
    std::size_t pushCount = 0;
    std::size_t popCount = 0;
    std::size_t unbalancedPops = 0;  ///< A pop with an empty stack: always a bug.
    std::size_t flushCount = 0;
    std::size_t drawImageCount = 0;
    /// Objects created and never released - a leak when non-zero.
    std::size_t liveObjects = 0;
    DRAWBOT_AntiAliasPolicy antiAlias = kDRAWBOT_AntiAliasPolicy_Default;

    /// Every vertex of every path, flattened, for the common case of "was a
    /// line drawn anywhere near here".
    [[nodiscard]] std::vector<DrawbotVertex> allVertices() const {
        std::vector<DrawbotVertex> out;
        for (const DrawbotPathRecord& path : paths) {
            out.insert(out.end(), path.vertices.begin(), path.vertices.end());
        }
        return out;
    }

    /// How many vertices of a given kind were recorded across every path.
    [[nodiscard]] std::size_t countVertices(DrawbotVertexKind kind) const {
        std::size_t n = 0;
        for (const DrawbotPathRecord& path : paths) {
            for (const DrawbotVertex& v : path.vertices) {
                if (v.kind == kind) {
                    ++n;
                }
            }
        }
        return n;
    }
};

/// Counters of the PPix cache.
struct CacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t entries = 0;
    std::size_t evictions = 0;
    std::size_t capacity = 0;
};

/// A pixel buffer wrapped as an AE effect world (top-left origin).  Owned
/// by the caller; the MockHost keeps a registry entry while it exists so
/// PF_PixelFormatSuite::GetPixelFormat can answer for it.
class EffectWorld {
public:
    ~EffectWorld();
    EffectWorld(const EffectWorld&) = delete;
    EffectWorld& operator=(const EffectWorld&) = delete;

    /// The world handed to the effect (params[0]->u.ld or output).
    [[nodiscard]] PF_EffectWorld& world() noexcept { return m_world; }
    [[nodiscard]] const PF_EffectWorld& world() const noexcept { return m_world; }
    [[nodiscard]] PrPixelFormat format() const noexcept { return m_format; }
    [[nodiscard]] std::uint32_t width() const noexcept { return m_width; }
    [[nodiscard]] std::uint32_t height() const noexcept { return m_height; }
    [[nodiscard]] std::int32_t rowBytes() const noexcept { return m_rowBytes; }
    /// Row 0 (top).
    [[nodiscard]] char* pixels() noexcept { return m_pixels; }
    [[nodiscard]] const char* pixels() const noexcept { return m_pixels; }

private:
    friend class MockHost;
    EffectWorld() = default;
    PF_EffectWorld m_world{};
    PrPixelFormat m_format = PrPixelFormat_Invalid;
    std::uint32_t m_width = 0;
    std::uint32_t m_height = 0;
    std::int32_t m_rowBytes = 0;
    char* m_pixels = nullptr;
    std::vector<std::uint8_t> m_storage;
};

/// Parameters for makeInData().
struct InDataSpec {
    A_long width = 1920;
    A_long height = 1080;
    A_long currentTime = 0;
    A_long timeStep = 1;
    A_long totalTime = 100;
    A_u_long timeScale = 30;
    A_long downsampleX = 1;
    A_long downsampleY = 1;
    A_long parNum = 1;
    A_long parDen = 1;
    PF_Quality quality = PF_Quality_HI;
    PF_Field field = PF_Field_FRAME;
};

class MockHost {
public:
    MockHost();
    ~MockHost();
    MockHost(const MockHost&) = delete;
    MockHost& operator=(const MockHost&) = delete;

    /// The live instance (nullptr when none).
    [[nodiscard]] static MockHost* current() noexcept;

    // ---- what a plug-in receives -------------------------------------------
    [[nodiscard]] piSuitesPtr piSuites() noexcept;
    [[nodiscard]] SPBasicSuite* basicSuite() noexcept;

    // ---- suite registry ----------------------------------------------------
    /// Hide or show a (name, version) pair; hidden suites make AcquireSuite
    /// return a null pointer like an older host would.
    void setSuiteAvailable(const char* name, int version, bool available);
    /// Outstanding acquire count for a pair (-1 when unknown).
    [[nodiscard]] int suiteRefCount(const char* name, int version) const;
    /// Sum of all outstanding acquires.
    [[nodiscard]] int totalSuiteRefs() const;

    // ---- legacy memory functions -------------------------------------------
    /// Live pointers + handles allocated through piSuites->memFuncs and the
    /// Memory Manager Suite.
    [[nodiscard]] std::size_t liveMemoryBlocks() const;

    // ---- PPix --------------------------------------------------------------
    /// Details of a PPix created by any of the mock suites (nullopt for an
    /// unknown or disposed handle).
    [[nodiscard]] std::optional<PPixInfo> inspect(PPixHand hand) const;
    /// Number of PPixes not yet fully disposed.
    [[nodiscard]] std::size_t livePPixCount() const;

    // ---- PPix cache --------------------------------------------------------
    [[nodiscard]] CacheStats cacheStats() const;
    /// Maximum number of frames kept (default 16); shrinking evicts.
    void setCacheCapacity(std::size_t capacity);
    /// Drop every cached frame.
    void clearCache();

    // ---- strings -----------------------------------------------------------
    /// UTF-8 contents of a PrSDKString allocated through the String Suite
    /// (empty for an unknown string).
    [[nodiscard]] std::string utf8(const PrSDKString& s) const;
    /// Allocate a PrSDKString the way the host would (tests use it to fill
    /// input fields such as selectedColorProfileName).
    [[nodiscard]] PrSDKString makeString(std::string_view utf8Text);
    /// Strings allocated and not yet disposed.
    [[nodiscard]] std::size_t liveStringCount() const;

    // ---- app info ----------------------------------------------------------
    void setAppIdentity(const AppIdentity& identity);
    [[nodiscard]] AppIdentity appIdentity() const;

    // ---- error suite -------------------------------------------------------
    [[nodiscard]] std::vector<ErrorEvent> errorEvents() const;
    void clearErrorEvents();

    // ---- colour management -------------------------------------------------
    /// Synthetic PrSDKColorSpaceID for a predefined token from
    /// PrSDKColorSpaces.h (kPrSDKColorSpaceID_Invalid for an unknown name).
    [[nodiscard]] PrSDKColorSpaceID colorSpaceId(std::string_view predefinedName) const;
    /// Reverse lookup ("" when unknown).
    [[nodiscard]] std::string predefinedName(const PrSDKColorSpaceID& id) const;
    /// SEI codes for a predefined token (defaults for unknown names).
    [[nodiscard]] prSEIColorCodesRec seiCodes(std::string_view predefinedName) const;

    // ---- sequence info -----------------------------------------------------
    /// Configuration of a timeline, created with defaults on first access.
    /// Mutate the returned copy and pass it to setSequence().
    [[nodiscard]] SequenceConfig sequence(PrTimelineID timeline) const;
    void setSequence(PrTimelineID timeline, const SequenceConfig& config);
    /// Forget a timeline (GetFrameRect etc. then return suiteError_IDNotValid).
    void removeSequence(PrTimelineID timeline);

    // ---- video segment -----------------------------------------------------
    /// Set a keyframe.  Float32 / Float64 / Point values interpolate
    /// linearly between keyframes, integer and Bool values hold.
    void setParam(csSDK_int32 nodeId, csSDK_int32 index, PrTime time, const PrParam& value);
    /// Remove every keyframe (and property) of a node.
    void clearNode(csSDK_int32 nodeId);
    /// Node property served by GetNodeProperty (UTF-8).
    void setNodeProperty(csSDK_int32 nodeId, std::string_view key, std::string_view value);

    // ---- GPU ---------------------------------------------------------------
    /// True when the CUDA driver initialised and a device exists.
    [[nodiscard]] bool gpuAvailable();
    [[nodiscard]] csSDK_uint32 gpuDeviceCount();
    /// Why gpuAvailable() is false ("" when it is true).
    [[nodiscard]] std::string gpuFailureReason();

    // ---- After Effects side ------------------------------------------------
    /// Create an opaque effect reference for PF_InData::effect_ref.
    [[nodiscard]] PF_ProgPtr createEffectRef(PrTimelineID timeline, A_long filterInstanceId);
    void destroyEffectRef(PF_ProgPtr ref);

    /// A PF_InData with pica_basicP, appl_id 'PrMr', working interaction
    /// callbacks and the geometry / timing from `spec`.
    [[nodiscard]] PF_InData makeInData(PF_ProgPtr ref, const InDataSpec& spec) const;
    /// A zeroed PF_OutData.
    [[nodiscard]] PF_OutData makeOutData() const;

    /// Allocate a top-left effect world of the given format
    /// (BGRA_4444_32f, BGRA_4444_8u, ARGB_4444_8u); row bytes are rounded up
    /// to 64.  Returns nullptr for an unsupported format or size.
    [[nodiscard]] std::unique_ptr<EffectWorld> createWorld(std::uint32_t width, std::uint32_t height, PrPixelFormat format);

    /// Pixel formats registered by the effect through
    /// PF_PixelFormatSuite::AddSupportedPixelFormat, in order.
    [[nodiscard]] std::vector<PrPixelFormat> supportedPixelFormats(PF_ProgPtr ref) const;

    /// Parameters the effect added through PF_ADD_PARAM (index 1..n).
    [[nodiscard]] std::vector<PF_ParamDef> addedParams(PF_ProgPtr ref) const;
    /// Override the value checkout_param returns for `index` (1-based; the
    /// def is copied whole, so set u.fs_d.value, u.pd.value, ... first).
    void setParamValue(PF_ProgPtr ref, A_long index, const PF_ParamDef& def);
    /// Install an AE-side KEYFRAME: checkout_param(index, time) returns `def`
    /// at exactly that time and the value set by setParamValue at every
    /// other.  The mock does not interpolate, so a test sees precisely the
    /// values it installed - which is what makes an averaging assertion mean
    /// something (with one value for all times, any normalised combination of
    /// three samples returns it).
    void setParamValueAtTime(PF_ProgPtr ref, A_long index, A_long time, const PF_ParamDef& def);
    /// Drop every keyframe installed on `ref`.
    void clearParamKeyframes(PF_ProgPtr ref);
    /// The world checkout_param(0) and params[0] wrap.
    void setInputWorld(PF_ProgPtr ref, EffectWorld* world);
    /// A ready-to-use params array for PF_Cmd_RENDER: [0] the input layer,
    /// [1..n] the added params with their current values.  The pointers
    /// stay valid until the next call on the same effect ref.
    [[nodiscard]] std::vector<PF_ParamDef*> renderParams(PF_ProgPtr ref);

    // ---- custom UI / DrawBot -----------------------------------------------
    /// The PF_CustomUIInfo the effect registered through register_ui during
    /// PF_Cmd_PARAMS_SETUP, or nullopt when it registered none.
    [[nodiscard]] std::optional<PF_CustomUIInfo> registeredCustomUi(PF_ProgPtr ref) const;

    /// A PF_ContextH for a custom-UI event.  The handle stays valid until
    /// the next call; the mock owns the PF_Context behind it.
    ///
    /// PF_Window_COMP is the Program Monitor, which is the only window the
    /// reframe overlay asks for (PF_CustomEFlag_COMP).
    [[nodiscard]] PF_ContextH makeCustomUiContext(PF_WindowType windowType = PF_Window_COMP);

    /// Whether the mock's DrawBot supplier reports text support.  Off makes
    /// SupportsText answer false and NewDefaultFont fail, which is a real
    /// possibility on some hosts and must not stop the graphics drawing.
    void setDrawbotSupportsText(bool supports);

    /// Whether PF_GetDrawingReference hands back a reference at all.  Off is
    /// the "no DrawBot" case: the plug-in must return cleanly and draw
    /// nothing.
    void setDrawbotProvidesDrawRef(bool provides);

    /// Make NewPen and/or NewPath fail, so a test can prove the drawing code
    /// checks every result instead of dereferencing a null.
    void setDrawbotFailures(bool failNewPen, bool failNewPath);

    /// Everything drawn since the last clearDrawbotRecord().
    [[nodiscard]] DrawbotRecord drawbotRecord() const;
    /// Forget every recorded draw call and object.
    void clearDrawbotRecord();

    struct Impl;
    /// Internal: the state object the suite functions operate on.
    [[nodiscard]] Impl* implForSuites() noexcept { return m_impl.get(); }

private:
    std::unique_ptr<Impl> m_impl;
};

}  // namespace osv::premiere::mock
