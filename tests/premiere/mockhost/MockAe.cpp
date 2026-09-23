// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// The After Effects side of the mock host: the PF Pixel Format Suite v1, the
// PF Utility Suite v4..v13, the PF Param Utils Suite v3 (PF_UpdateParamUI
// recorded) and the PF_InData / PF_OutData builders with
// working interaction callbacks (checkout_param, checkin_param, add_param,
// abort, progress).
//
// What a Premiere-hosted AE effect actually touches during a test run is
// small and well defined:
//
//   * PF_Cmd_GLOBAL_SETUP registers pixel formats through
//     PF_PixelFormatSuite1::AddSupportedPixelFormat, which lands in the
//     EffectRef's format list and is read back with supportedPixelFormats();
//   * PF_Cmd_PARAMS_SETUP adds parameters with PF_ADD_PARAM, which the mock
//     stores verbatim (index 1..n) so a test can assert the IDs, ranges and
//     defaults;
//   * PF_Cmd_RENDER is handed params[0] (the input layer, whose u.ld is the
//     PF_EffectWorld of the world set with setInputWorld) and params[1..n]
//     (the values set with setParamValue), plus an output world;
//   * checkout_param at a neighbouring time returns exactly the stored value
//     (AE-side keyframes installed with setParamValueAtTime are returned at
//     exactly their time and never interpolated - the GPU path reads
//     keyframes through the Video Segment Suite instead, which does);
//   * PF_FindKeyframeTime / PF_GetKeyframeCount / PF_KeyIndexToTime answer
//     from those same installed keyframes, which is how the Keyframe Easing
//     finds the interval around the render time on the CPU path.
//
// Effect worlds are top-left origin with positive row bytes rounded up to 64,
// which is what Premiere hands an AE-API effect.  A world registers itself in
// Impl::worlds while it lives so GetPixelFormat can answer for it.

#include "MockHostImpl.h"

// PF_PLUG_IN_VERSION / PF_PLUG_IN_SUBVERS (the effect spec version the host
// reports in PF_InData::version) live here, not in AE_Effect.h.
#include "AE_EffectVers.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <utility>

namespace osv::premiere::mock {

namespace {

MockHost::Impl* impl() noexcept {
    MockHost* h = MockHost::current();
    return h ? h->implForSuites() : nullptr;
}

/// Row pitch of an effect world: 64-byte multiples, always positive.
std::int32_t worldRowBytes(std::uint32_t width, std::size_t bpp) noexcept {
    const std::size_t raw = static_cast<std::size_t>(width) * bpp;
    return static_cast<std::int32_t>((raw + 63u) & ~static_cast<std::size_t>(63u));
}

// -----------------------------------------------------------------------------
//  PF Pixel Format Suite v1
// -----------------------------------------------------------------------------
PF_Err pfAddSupportedPixelFormat(PF_ProgPtr effectRef, PrPixelFormat format) {
    MockHost::Impl* p = impl();
    if (!p) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // The host keeps the registration order: the first format registered is
    // the effect's preferred one.
    ref->formats.push_back(format);
    return PF_Err_NONE;
}

PF_Err pfClearSupportedPixelFormats(PF_ProgPtr effectRef) {
    MockHost::Impl* p = impl();
    if (!p) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    ref->formats.clear();
    return PF_Err_NONE;
}

PF_Err pfNewWorldOfPixelFormat(PF_ProgPtr effectRef, A_u_long width, A_u_long height, PF_NewWorldFlags,
                               PrPixelFormat format, PF_EffectWorld* world) {
    MockHost* host = MockHost::current();
    MockHost::Impl* p = impl();
    if (!host || !p || !world) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->effectRef(effectRef)) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // The world is owned by the host until the MockHost is destroyed (or
    // DisposeWorld is called), exactly like a real allocation from this
    // suite.
    std::unique_ptr<EffectWorld> created =
        host->createWorld(static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height), format);
    if (!created) {
        return PF_Err_OUT_OF_MEMORY;
    }
    *world = created->world();
    p->hostWorlds.push_back(std::move(created));
    return PF_Err_NONE;
}

PF_Err pfDisposeWorld(PF_ProgPtr effectRef, PF_EffectWorld* world) {
    MockHost::Impl* p = impl();
    if (!p || !world) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->effectRef(effectRef)) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // Find the owning world by its pixel address; a world the caller owns is
    // not ours to free, so it is simply ignored.
    for (auto it = p->hostWorlds.begin(); it != p->hostWorlds.end(); ++it) {
        if ((*it) && (*it)->world().data == world->data) {
            p->hostWorlds.erase(it);
            std::memset(world, 0, sizeof(*world));
            return PF_Err_NONE;
        }
    }
    return PF_Err_NONE;
}

PF_Err pfGetPixelFormat(PF_EffectWorld* world, PrPixelFormat* format) {
    MockHost::Impl* p = impl();
    if (!p || !world || !format) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    auto it = p->worlds.find(world);
    if (it == p->worlds.end() || !it->second) {
        // Look the world up by its data pointer too: the effect is handed a
        // copy of the PF_EffectWorld inside a PF_ParamDef, so the address of
        // the struct differs from the registered one.
        for (const auto& entry : p->worlds) {
            if (entry.second && entry.second->world().data == world->data) {
                *format = entry.second->format();
                return PF_Err_NONE;
            }
        }
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *format = it->second->format();
    return PF_Err_NONE;
}

/// Black / white in the requested format, used by effects that clear a
/// buffer.  Exactly the formats createWorld() supports are answered, so the
/// two stay in step.
PF_Err pfFillPixel(PrPixelFormat format, float a, float r, float g, float b, void* pixel) {
    if (!pixel) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    switch (format) {
    // _32f_Linear differs from _32f only in transfer function, so the byte
    // layout - and therefore this store - is identical.
    case PrPixelFormat_BGRA_4444_32f:
    case PrPixelFormat_BGRA_4444_32f_Linear: {
        auto* f = static_cast<float*>(pixel);
        f[0] = b;
        f[1] = g;
        f[2] = r;
        f[3] = a;
        return PF_Err_NONE;
    }
    case PrPixelFormat_BGRA_4444_16u: {
        // White is 32768, not 65535 (Premiere SDK guide 5.4.2).  The mock
        // uses the documented scale so a test comparing against it is
        // testing the real convention rather than the mock's guess.
        auto* u = static_cast<std::uint16_t*>(pixel);
        const auto q = [](float v) {
            return static_cast<std::uint16_t>(std::clamp(v, 0.0f, 1.0f) * 32768.0f + 0.5f);
        };
        u[0] = q(b);
        u[1] = q(g);
        u[2] = q(r);
        u[3] = q(a);
        return PF_Err_NONE;
    }
    case PrPixelFormat_BGRA_4444_8u: {
        auto* u = static_cast<std::uint8_t*>(pixel);
        const auto q = [](float v) {
            return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        u[0] = q(b);
        u[1] = q(g);
        u[2] = q(r);
        u[3] = q(a);
        return PF_Err_NONE;
    }
    case PrPixelFormat_ARGB_4444_8u: {
        auto* u = static_cast<std::uint8_t*>(pixel);
        const auto q = [](float v) {
            return static_cast<std::uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
        };
        u[0] = q(a);
        u[1] = q(r);
        u[2] = q(g);
        u[3] = q(b);
        return PF_Err_NONE;
    }
    default:
        break;
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

PF_Err pfGetBlackForPixelFormat(PrPixelFormat format, void* pixel) {
    return pfFillPixel(format, 1.0f, 0.0f, 0.0f, 0.0f, pixel);
}

PF_Err pfGetWhiteForPixelFormat(PrPixelFormat format, void* pixel) {
    return pfFillPixel(format, 1.0f, 1.0f, 1.0f, 1.0f, pixel);
}

PF_Err pfConvertColorToPixelFormattedData(PrPixelFormat format, float alpha, float red, float green, float blue,
                                          void* pixel) {
    return pfFillPixel(format, alpha, red, green, blue, pixel);
}

// -----------------------------------------------------------------------------
//  PF Utility Suite v4 (the members the effect uses; the rest answer with a
//  defined value rather than a null pointer so a stray call cannot crash)
// -----------------------------------------------------------------------------
PF_Err pfUtilGetFilterInstanceID(PF_ProgPtr effectRef, A_long* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = ref->instanceId;
    return PF_Err_NONE;
}

PF_Err pfUtilGetMediaTimecode(PF_ProgPtr, A_long* outFrame, PF_TimeDisplay* outDisplay) {
    if (outFrame) {
        *outFrame = 0;
    }
    if (outDisplay) {
        *outDisplay = 0;
    }
    return PF_Err_NONE;
}

PF_Err pfUtilGetClipSpeed(PF_ProgPtr, double* out) {
    if (!out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = 1.0;
    return PF_Err_NONE;
}

PF_Err pfUtilGetFrameCount(PF_ProgPtr, A_long* out) {
    if (!out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = 0;
    return PF_Err_NONE;
}

PF_Err pfUtilGetMediaFieldType(PF_ProgPtr, prFieldType* out) {
    if (!out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = prFieldsNone;
    return PF_Err_NONE;
}

PF_Err pfUtilGetMediaFrameRate(PF_ProgPtr effectRef, PrTime* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // From the timeline the effect sits on when the test configured one.
    auto it = p->sequences.find(ref->timeline);
    *out = it != p->sequences.end() ? it->second.ticksPerFrame : (kTicksPerSecond / 30);
    return PF_Err_NONE;
}

PF_Err pfUtilGetContainingTimelineID(PF_ProgPtr effectRef, PrTimelineID* out) {
    MockHost::Impl* p = impl();
    if (!p || !out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = ref->timeline;
    return PF_Err_NONE;
}

PF_Err pfUtilGetClipName(PF_ProgPtr effectRef, PrSDKString* out) {
    MockHost* host = MockHost::current();
    MockHost::Impl* p = impl();
    if (!host || !p || !out) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    if (!p->effectRef(effectRef)) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *out = host->makeString("MockClip");
    return PF_Err_NONE;
}

PF_Err pfUtilEffectWantsMatchingFormat(PF_ProgPtr effectRef) {
    MockHost::Impl* p = impl();
    if (!p) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->effectRef(effectRef) ? PF_Err_NONE : PF_Err_BAD_CALLBACK_PARAM;
}

// -----------------------------------------------------------------------------
//  PF_InData interaction callbacks
// -----------------------------------------------------------------------------

/// checkout_param: index 0 is the input layer, 1..n the added parameters.
///
/// `time` is honoured when the test installed a keyframe for exactly that
/// (index, time) with MockHost::setParamValueAtTime; otherwise the stored
/// value is returned for every time, which is the behaviour every existing
/// test relies on.  Only exact times match, deliberately: the mock does not
/// interpolate, so a test asserting an average sees exactly the three values
/// it installed and nothing invented in between.
PF_Err interCheckoutParam(PF_ProgPtr effectRef, PF_ParamIndex index, A_long time, A_long, A_u_long,
                          PF_ParamDef* param) {
    MockHost::Impl* p = impl();
    if (!p || !param) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    if (index == 0) {
        *param = ref->inputLayer;
        return PF_Err_NONE;
    }
    if (index < 1 || static_cast<std::size_t>(index) > ref->params.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    const auto keyed = ref->keyframes.find(std::make_pair(static_cast<A_long>(index), time));
    if (keyed != ref->keyframes.end()) {
        *param = keyed->second;
        return PF_Err_NONE;
    }
    *param = ref->params[static_cast<std::size_t>(index) - 1u];
    return PF_Err_NONE;
}

/// checkin_param: nothing is locked, so this only validates the arguments.
PF_Err interCheckinParam(PF_ProgPtr effectRef, PF_ParamDef* param) {
    MockHost::Impl* p = impl();
    if (!p || !param) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    return p->effectRef(effectRef) ? PF_Err_NONE : PF_Err_BAD_CALLBACK_PARAM;
}

/// add_param: index -1 appends, which is what every effect uses.
PF_Err interAddParam(PF_ProgPtr effectRef, PF_ParamIndex index, PF_ParamDefPtr def) {
    MockHost::Impl* p = impl();
    if (!p || !def) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    if (index < 0 || static_cast<std::size_t>(index) >= ref->params.size()) {
        ref->params.push_back(*def);
    } else {
        ref->params.insert(ref->params.begin() + index, *def);
    }
    return PF_Err_NONE;
}

/// abort: the mock never asks an effect to stop.
PF_Err interAbort(PF_ProgPtr) { return PF_Err_NONE; }

/// progress: accepted and discarded, returns 0 = keep going.
PF_Err interProgress(PF_ProgPtr, A_long, A_long) { return PF_Err_NONE; }

/// register_ui: RECORD the custom UI the effect asked for.
///
/// The reframe effect registers a Program Monitor overlay here
/// (PF_CustomEFlag_COMP), and the only way a test can prove it asked for the
/// right window - and for the right comp UI size - is for the mock to keep
/// the PF_CustomUIInfo it was handed.  A null info is rejected rather than
/// stored, because a host that got one would be looking at garbage.
PF_Err interRegisterUi(PF_ProgPtr effectRef, PF_CustomUIInfo* info) {
    MockHost::Impl* p = impl();
    if (!p || !info) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    ref->customUi = *info;
    ref->customUiRegistered = true;
    return PF_Err_NONE;
}

}  // namespace

// -----------------------------------------------------------------------------
//  EffectWorld
// -----------------------------------------------------------------------------
EffectWorld::~EffectWorld() {
    // Unregister so GetPixelFormat stops answering for a dead world.
    if (MockHost* host = MockHost::current()) {
        if (MockHost::Impl* p = host->implForSuites()) {
            std::lock_guard<std::recursive_mutex> lock(p->mutex);
            p->worlds.erase(&m_world);
        }
    }
}

// -----------------------------------------------------------------------------
//  MockHost API backed by this file
// -----------------------------------------------------------------------------
PF_ProgPtr MockHost::createEffectRef(PrTimelineID timeline, A_long filterInstanceId) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    auto* ref = new (std::nothrow) EffectRef();
    if (!ref) {
        return nullptr;
    }
    ref->timeline = timeline;
    ref->instanceId = filterInstanceId;
    // params[0] is the input layer; it is a PF_Param_LAYER whose u.ld is
    // filled by setInputWorld().
    std::memset(&ref->inputLayer, 0, sizeof(ref->inputLayer));
    ref->inputLayer.param_type = PF_Param_LAYER;
    m_impl->effectRefs.insert(ref);
    return reinterpret_cast<PF_ProgPtr>(ref);
}

void MockHost::destroyEffectRef(PF_ProgPtr ref) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return;
    }
    m_impl->effectRefs.erase(r);
    r->magic = 0;
    delete r;
}

PF_InData MockHost::makeInData(PF_ProgPtr ref, const InDataSpec& spec) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    PF_InData in{};

    // Interaction callbacks: everything an effect may call during
    // GLOBAL_SETUP / PARAMS_SETUP / RENDER.
    in.inter.checkout_param = &interCheckoutParam;
    in.inter.checkin_param = &interCheckinParam;
    in.inter.add_param = &interAddParam;
    in.inter.abort = &interAbort;
    in.inter.progress = &interProgress;
    in.inter.register_ui = &interRegisterUi;

    in.effect_ref = ref;
    in.quality = spec.quality;
    in.version.major = PF_PLUG_IN_VERSION;
    in.version.minor = PF_PLUG_IN_SUBVERS;
    in.serial_num = 1;
    // Premiere identifies itself with 'PrMr'; the effect branches on this.
    in.appl_id = kAppID_Premiere;
    const EffectRef* r = m_impl->effectRef(ref);
    in.num_params = r ? static_cast<A_long>(r->params.size()) + 1 : 1;
    in.current_time = spec.currentTime;
    in.time_step = spec.timeStep;
    in.total_time = spec.totalTime;
    in.local_time_step = spec.timeStep;
    in.time_scale = spec.timeScale;
    in.field = spec.field;
    in.width = spec.width;
    in.height = spec.height;
    in.extent_hint.left = 0;
    in.extent_hint.top = 0;
    in.extent_hint.right = static_cast<A_short>(spec.width);
    in.extent_hint.bottom = static_cast<A_short>(spec.height);
    // Downsample factors are num/den: 1/1 = full resolution, 1/2 = half.
    in.downsample_x.num = 1;
    in.downsample_x.den = static_cast<A_u_long>(spec.downsampleX > 0 ? spec.downsampleX : 1);
    in.downsample_y.num = 1;
    in.downsample_y.den = static_cast<A_u_long>(spec.downsampleY > 0 ? spec.downsampleY : 1);
    in.pixel_aspect_ratio.num = spec.parNum;
    in.pixel_aspect_ratio.den = static_cast<A_u_long>(spec.parDen > 0 ? spec.parDen : 1);
    in.pica_basicP = &m_impl->basic;
    return in;
}

PF_OutData MockHost::makeOutData() const {
    PF_OutData out{};
    return out;
}

std::unique_ptr<EffectWorld> MockHost::createWorld(std::uint32_t width, std::uint32_t height, PrPixelFormat format) {
    // Only the formats a Premiere-hosted AE effect is handed.
    //
    // BGRA_4444_16u and BGRA_4444_32f_Linear are here because a real
    // Premiere hands them to an effect on a high-bit-depth sequence - 16u on
    // a 10-bit timeline in particular, which is exactly the configuration
    // where the effect's CPU path used to refuse every frame.  A mock that
    // cannot build such a world cannot test that path at all.
    if (format != PrPixelFormat_BGRA_4444_32f && format != PrPixelFormat_BGRA_4444_32f_Linear &&
        format != PrPixelFormat_BGRA_4444_16u && format != PrPixelFormat_BGRA_4444_8u &&
        format != PrPixelFormat_ARGB_4444_8u) {
        return nullptr;
    }
    const std::size_t bpp = bytesPerPixel(format);
    if (bpp == 0 || width == 0 || height == 0 || width > 32768u || height > 32768u) {
        return nullptr;
    }

    std::unique_ptr<EffectWorld> world(new (std::nothrow) EffectWorld());
    if (!world) {
        return nullptr;
    }
    world->m_format = format;
    world->m_width = width;
    world->m_height = height;
    world->m_rowBytes = worldRowBytes(width, bpp);

    const std::size_t bytes = static_cast<std::size_t>(world->m_rowBytes) * height;
    try {
        // 64 spare bytes so the first row can start 64-byte aligned.
        world->m_storage.assign(bytes + 64u, 0u);
    } catch (...) {
        return nullptr;
    }
    const std::uintptr_t raw = reinterpret_cast<std::uintptr_t>(world->m_storage.data());
    const std::uintptr_t aligned = (raw + 63u) & ~static_cast<std::uintptr_t>(63u);
    world->m_pixels = reinterpret_cast<char*>(aligned);

    // The PF_EffectWorld the effect receives: top-left origin, positive
    // pitch, DEEP set for every world carrying more than 8 bits per channel
    // (AE's own convention), which is the two float formats and 16u - not
    // just 32f.  Deriving it from the byte width rather than listing formats
    // means a format added above cannot be left mislabelled.
    world->m_world.data = reinterpret_cast<PF_PixelPtr>(world->m_pixels);
    world->m_world.rowbytes = world->m_rowBytes;
    world->m_world.width = static_cast<A_long>(width);
    world->m_world.height = static_cast<A_long>(height);
    world->m_world.world_flags = (bpp > 4u) ? PF_WorldFlag_DEEP : 0;
    world->m_world.extent_hint.left = 0;
    world->m_world.extent_hint.top = 0;
    world->m_world.extent_hint.right = static_cast<A_long>(width);
    world->m_world.extent_hint.bottom = static_cast<A_long>(height);
    world->m_world.pix_aspect_ratio.num = 1;
    world->m_world.pix_aspect_ratio.den = 1;

    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    m_impl->worlds[&world->m_world] = world.get();
    return world;
}

bool MockHost::setWorldFormat(const PF_EffectWorld* world, PrPixelFormat format) {
    if (!world) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const auto it = m_impl->worlds.find(world);
    if (it == m_impl->worlds.end() || !it->second) {
        return false;
    }
    // The label only.  The buffer, its size and its pitch are deliberately
    // left exactly as allocated, which is the whole point: the effect must
    // decide from the FORMAT, not from the geometry.
    it->second->m_format = format;
    return true;
}

std::vector<PrPixelFormat> MockHost::supportedPixelFormats(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->formats : std::vector<PrPixelFormat>{};
}

std::optional<PF_CustomUIInfo> MockHost::registeredCustomUi(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    if (!r || !r->customUiRegistered) {
        return std::nullopt;
    }
    return r->customUi;
}

std::vector<PF_ParamDef> MockHost::addedParams(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->params : std::vector<PF_ParamDef>{};
}

void MockHost::setParamValue(PF_ProgPtr ref, A_long index, const PF_ParamDef& def) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r || index < 1 || static_cast<std::size_t>(index) > r->params.size()) {
        return;
    }
    r->params[static_cast<std::size_t>(index) - 1u] = def;
}

void MockHost::setParamValueAtTime(PF_ProgPtr ref, A_long index, A_long time, const PF_ParamDef& def) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r || index < 1 || static_cast<std::size_t>(index) > r->params.size()) {
        return;
    }
    r->keyframes[std::make_pair(index, time)] = def;
}

void MockHost::clearParamKeyframes(PF_ProgPtr ref) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return;
    }
    r->keyframes.clear();
}

std::size_t MockHost::findKeyframeCalls(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->findKeyframeCalls : 0u;
}

void MockHost::setInputWorld(PF_ProgPtr ref, EffectWorld* world) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return;
    }
    r->input = world;
    std::memset(&r->inputLayer, 0, sizeof(r->inputLayer));
    r->inputLayer.param_type = PF_Param_LAYER;
    if (world) {
        r->inputLayer.u.ld = world->world();
    }
}

std::vector<PF_ParamDef*> MockHost::renderParams(PF_ProgPtr ref) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return {};
    }
    // The array points straight at the stored defs, so an effect that writes
    // back a change flag is visible to the test afterwards.  It is rebuilt on
    // every call because params may have been added since the last one.
    r->renderArray.clear();
    r->renderArray.reserve(r->params.size() + 1u);
    r->renderArray.push_back(&r->inputLayer);
    for (PF_ParamDef& def : r->params) {
        r->renderArray.push_back(&def);
    }
    return r->renderArray;
}

// -----------------------------------------------------------------------------
//  PF Source Settings Suite (v1 == v2)
// -----------------------------------------------------------------------------
//
//  Two members, and between them they are the whole source settings contract:
//  SetIsSourceSettingsEffect is how an effect tells the host it belongs on a
//  master clip, and PerformSourceSettingsCommand is the private byte channel
//  to the matching importer.  Neither leaves a trace anywhere a test could
//  otherwise look, so both are recorded on the EffectRef.
//
//  PF_SourceSettingsSuite2 is a typedef of PF_SourceSettingsSuite
//  (PrSDKAESupport.h:1637) - the two versions have identical layouts - so one
//  table is registered for both, exactly as the real host must.

namespace {

PF_Err pfSourceSettingsSetIsSourceSettingsEffect(PF_ProgPtr effectRef, A_Boolean isSourceSettings) {
    MockHost::Impl* p = impl();
    if (!p) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    ref->isSourceSettingsEffect = (isSourceSettings != 0);
    ref->sourceSettingsFlagSet = true;
    return PF_Err_NONE;
}

PF_Err pfSourceSettingsPerformCommand(PF_ProgPtr effectRef, void* ioCommandStruct, csSDK_uint32 inDataSize) {
    MockHost::Impl* p = impl();
    if (!p) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // A null buffer or a zero size is the caller's bug, and a real host would
    // reject it rather than routing it to an importer.  Recording the call
    // anyway lets a test assert that the effect did not make it.
    ++ref->sourceSettingsCallCount;
    ref->sourceSettingsSentSize = inDataSize;
    if (!ioCommandStruct || inDataSize == 0) {
        ref->sourceSettingsSent.clear();
        return PF_Err_BAD_CALLBACK_PARAM;
    }

    // Snapshot what the effect sent BEFORE any reply overwrites it: a test
    // asserting "the effect seeded the buffer with its own controls" has to
    // read the outbound bytes, which the reply would otherwise destroy.
    const char* sent = static_cast<const char*>(ioCommandStruct);
    ref->sourceSettingsSent.assign(sent, sent + inDataSize);

    if (ref->sourceSettingsError != PF_Err_NONE) {
        // A failing host call must leave the buffer alone; an effect that
        // reads it anyway is the bug this path exists to catch.
        return ref->sourceSettingsError;
    }

    if (ref->sourceSettingsReplies && !ref->sourceSettingsReply.empty()) {
        // Truncate to the buffer the effect declared.  Writing more would be
        // the overflow a real importer must never commit, and a mock that
        // could commit it would hide the bug rather than expose it.
        const std::size_t n = std::min(ref->sourceSettingsReply.size(), static_cast<std::size_t>(inDataSize));
        std::memcpy(ioCommandStruct, ref->sourceSettingsReply.data(), n);
    }
    return PF_Err_NONE;
}

}  // namespace

std::optional<bool> MockHost::isSourceSettingsEffect(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    if (!r || !r->sourceSettingsFlagSet) {
        return std::nullopt;
    }
    return r->isSourceSettingsEffect;
}

std::vector<char> MockHost::sourceSettingsSentData(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->sourceSettingsSent : std::vector<char>{};
}

csSDK_uint32 MockHost::sourceSettingsSentSize(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->sourceSettingsSentSize : 0u;
}

std::size_t MockHost::sourceSettingsCallCount(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->sourceSettingsCallCount : 0u;
}

void MockHost::setSourceSettingsReply(PF_ProgPtr ref, const std::vector<char>& reply) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return;
    }
    r->sourceSettingsReply = reply;
    // An empty reply means "behave like a host with no clip instance", which
    // is a distinct, meaningful state rather than an empty write.
    r->sourceSettingsReplies = !reply.empty();
}

void MockHost::setSourceSettingsError(PF_ProgPtr ref, PF_Err err) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (r) {
        r->sourceSettingsError = err;
    }
}

// -----------------------------------------------------------------------------
//  PF Param Utils Suite v3
// -----------------------------------------------------------------------------
//
//  PF_UpdateParamUI is RECORDED: in a real host it only changes how the
//  Effect Controls panel draws a control, so the mock keeps each accepted
//  call on the EffectRef and a test reads back which controls an effect hid
//  or showed.  Only the fields the SDK lets the call change are copied
//  (AE_EffectSuites.h: ui_flags, the name, flags, and the slider display of a
//  float slider).  Every other member answers with a defined error rather
//  than a null pointer, so a stray call cannot crash a test.

namespace {

PF_Err pfParamUtilsUpdateParamUI(PF_ProgPtr effectRef, PF_ParamIndex index, const PF_ParamDef* def) {
    MockHost::Impl* p = impl();
    if (!p || !def) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // Index 0 is the input layer, which has no UI to update; past the list
    // is a control the effect never added.  A real host refuses both.
    if (index < 1 || static_cast<std::size_t>(index) > ref->params.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    // An injected refusal is returned before anything is recorded.
    const auto refused = ref->uiErrors.find(static_cast<A_long>(index));
    if (refused != ref->uiErrors.end()) {
        return refused->second;
    }

    ParamUiUpdate u;
    u.index = static_cast<A_long>(index);
    u.type = def->param_type;
    u.uiFlags = def->ui_flags;
    u.flags = def->flags;
    // The name buffer is a fixed array; copy up to its NUL or its end, so a
    // def whose name was never terminated cannot read past the struct.
    const char* name = def->PF_DEF_NAME;
    std::size_t length = 0;
    while (length < sizeof(def->PF_DEF_NAME) && name[length] != '\0') {
        ++length;
    }
    u.name.assign(name, length);
    if (def->param_type == PF_Param_FLOAT_SLIDER) {
        u.sliderMin = static_cast<float>(def->u.fs_d.slider_min);
        u.sliderMax = static_cast<float>(def->u.fs_d.slider_max);
        u.precision = def->u.fs_d.precision;
        u.displayFlags = def->u.fs_d.display_flags;
    }
    ref->uiUpdates.push_back(std::move(u));
    return PF_Err_NONE;
}

PF_Err pfParamUtilsGetCurrentState(PF_ProgPtr, PF_ParamIndex, const A_Time*, const A_Time*, PF_State* state) {
    if (state) {
        std::memset(state, 0, sizeof(*state));
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

PF_Err pfParamUtilsAreStatesIdentical(PF_ProgPtr, const PF_State*, const PF_State*, A_Boolean* same) {
    if (same) {
        *same = FALSE;
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

PF_Err pfParamUtilsIsIdenticalCheckout(PF_ProgPtr, PF_ParamIndex, A_long, A_long, A_u_long, A_long, A_long,
                                       A_u_long, PF_Boolean* identical) {
    if (identical) {
        *identical = FALSE;
    }
    return PF_Err_BAD_CALLBACK_PARAM;
}

/// The AE-side keyframe times of one parameter, ascending: the times the
/// test installed with MockHost::setParamValueAtTime.  They are in whatever
/// time scale the test used - the mock has one scale per effect, the one its
/// PF_InData reports - so the scale argument of the queries below is echoed
/// back rather than converted.
std::vector<A_long> keyframeTimesLocked(const EffectRef& ref, PF_ParamIndex index) {
    std::vector<A_long> times;
    for (const auto& entry : ref.keyframes) {
        if (entry.first.first == static_cast<A_long>(index)) {
            times.push_back(entry.first.second);  // the map is ordered, so ascending
        }
    }
    return times;
}

/// PF_FindKeyframeTime (the [WP-EASING] Keyframe Easing reads keyframes
/// through it): the keyframe nearest `what_time` in the direction asked,
/// from the installed keyframes.  A parameter with none is "not found",
/// which is what a host answers for a constant control.
PF_Err pfParamUtilsFindKeyframeTime(PF_ProgPtr effectRef, PF_ParamIndex index, A_long what_time, A_u_long time_scale,
                                    PF_TimeDir dir, PF_Boolean* found, PF_KeyIndex* key_index, A_long* key_time,
                                    A_u_long* key_timescale) {
    MockHost::Impl* p = impl();
    if (!p || !found) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *found = FALSE;
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    EffectRef* ref = p->effectRef(effectRef);
    if (!ref || index < 1 || static_cast<std::size_t>(index) > ref->params.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    ++ref->findKeyframeCalls;
    const std::vector<A_long> times = keyframeTimesLocked(*ref, index);
    // Walk the ascending list for the one keyframe the direction names.
    int pick = -1;
    for (int i = 0; i < static_cast<int>(times.size()); ++i) {
        const A_long k = times[static_cast<std::size_t>(i)];
        switch (dir) {
            case PF_TimeDir_LESS_THAN:
                pick = (k < what_time) ? i : pick;
                break;
            case PF_TimeDir_LESS_THAN_OR_EQUAL:
                pick = (k <= what_time) ? i : pick;
                break;
            case PF_TimeDir_GREATER_THAN:
                pick = (pick < 0 && k > what_time) ? i : pick;
                break;
            case PF_TimeDir_GREATER_THAN_OR_EQUAL:
                pick = (pick < 0 && k >= what_time) ? i : pick;
                break;
            default:
                return PF_Err_BAD_CALLBACK_PARAM;
        }
    }
    if (pick < 0) {
        return PF_Err_NONE;  // not found is an answer, not an error
    }
    *found = TRUE;
    if (key_index) {
        *key_index = pick;
    }
    if (key_time) {
        *key_time = times[static_cast<std::size_t>(pick)];
    }
    if (key_timescale) {
        *key_timescale = time_scale;
    }
    return PF_Err_NONE;
}

/// PF_GetKeyframeCount: PF_KeyIndex_NONE for a constant parameter, as the
/// header documents, otherwise the number installed.
PF_Err pfParamUtilsGetKeyframeCount(PF_ProgPtr effectRef, PF_ParamIndex index, PF_KeyIndex* count) {
    MockHost::Impl* p = impl();
    if (!p || !count) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const EffectRef* ref = p->effectRef(effectRef);
    if (!ref || index < 1 || static_cast<std::size_t>(index) > ref->params.size()) {
        *count = PF_KeyIndex_NONE;
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    const std::size_t n = keyframeTimesLocked(*ref, index).size();
    *count = (n == 0) ? PF_KeyIndex_NONE : static_cast<PF_KeyIndex>(n);
    return PF_Err_NONE;
}

PF_Err pfParamUtilsCheckoutKeyframe(PF_ProgPtr, PF_ParamIndex, PF_KeyIndex, A_long*, A_u_long*, PF_ParamDef*) {
    return PF_Err_BAD_CALLBACK_PARAM;
}

PF_Err pfParamUtilsCheckinKeyframe(PF_ProgPtr, PF_ParamDef*) { return PF_Err_BAD_CALLBACK_PARAM; }

/// PF_KeyIndexToTime: the n-th installed keyframe's time (0-based), in the
/// mock's one time scale (reported as 0: the mock has no scale of its own).
PF_Err pfParamUtilsKeyIndexToTime(PF_ProgPtr effectRef, PF_ParamIndex index, PF_KeyIndex key, A_long* time,
                                  A_u_long* scale) {
    MockHost::Impl* p = impl();
    if (!p || !time) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    std::lock_guard<std::recursive_mutex> lock(p->mutex);
    const EffectRef* ref = p->effectRef(effectRef);
    if (!ref || index < 1 || static_cast<std::size_t>(index) > ref->params.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    const std::vector<A_long> times = keyframeTimesLocked(*ref, index);
    if (key < 0 || static_cast<std::size_t>(key) >= times.size()) {
        return PF_Err_BAD_CALLBACK_PARAM;
    }
    *time = times[static_cast<std::size_t>(key)];
    if (scale) {
        *scale = 0;
    }
    return PF_Err_NONE;
}

}  // namespace

std::vector<ParamUiUpdate> MockHost::paramUiUpdates(PF_ProgPtr ref) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    return r ? r->uiUpdates : std::vector<ParamUiUpdate>{};
}

std::optional<ParamUiUpdate> MockHost::paramUiState(PF_ProgPtr ref, A_long index) const {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    const EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return std::nullopt;
    }
    // The latest call wins, exactly as the panel would show it.
    for (auto it = r->uiUpdates.rbegin(); it != r->uiUpdates.rend(); ++it) {
        if (it->index == index) {
            return *it;
        }
    }
    return std::nullopt;
}

void MockHost::clearParamUiUpdates(PF_ProgPtr ref) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    if (EffectRef* r = m_impl->effectRef(ref)) {
        r->uiUpdates.clear();
    }
}

void MockHost::setParamUiError(PF_ProgPtr ref, A_long index, PF_Err err) {
    std::lock_guard<std::recursive_mutex> lock(m_impl->mutex);
    EffectRef* r = m_impl->effectRef(ref);
    if (!r) {
        return;
    }
    if (err == PF_Err_NONE) {
        r->uiErrors.erase(index);
    } else {
        r->uiErrors[index] = err;
    }
}

// -----------------------------------------------------------------------------
//  Registration
// -----------------------------------------------------------------------------
void installAeSuites(MockHost::Impl& p) {
    p.pfPixelFormat.AddSupportedPixelFormat = &pfAddSupportedPixelFormat;
    p.pfPixelFormat.ClearSupportedPixelFormats = &pfClearSupportedPixelFormats;
    p.pfPixelFormat.NewWorldOfPixelFormat = &pfNewWorldOfPixelFormat;
    p.pfPixelFormat.DisposeWorld = &pfDisposeWorld;
    p.pfPixelFormat.GetPixelFormat = &pfGetPixelFormat;
    p.pfPixelFormat.GetBlackForPixelFormat = &pfGetBlackForPixelFormat;
    p.pfPixelFormat.GetWhiteForPixelFormat = &pfGetWhiteForPixelFormat;
    p.pfPixelFormat.ConvertColorToPixelFormattedData = &pfConvertColorToPixelFormattedData;
    p.registerSuite(kPFPixelFormatSuite, kPFPixelFormatSuiteVersion1, &p.pfPixelFormat);

    p.pfUtility.GetFilterInstanceID = &pfUtilGetFilterInstanceID;
    p.pfUtility.GetMediaTimecode = &pfUtilGetMediaTimecode;
    p.pfUtility.GetClipSpeed = &pfUtilGetClipSpeed;
    p.pfUtility.GetClipDuration = &pfUtilGetFrameCount;
    p.pfUtility.GetClipStart = &pfUtilGetFrameCount;
    p.pfUtility.GetUnscaledClipDuration = &pfUtilGetFrameCount;
    p.pfUtility.GetUnscaledClipStart = &pfUtilGetFrameCount;
    p.pfUtility.GetTrackItemStart = &pfUtilGetFrameCount;
    p.pfUtility.GetMediaFieldType = &pfUtilGetMediaFieldType;
    p.pfUtility.GetMediaFrameRate = &pfUtilGetMediaFrameRate;
    p.pfUtility.GetContainingTimelineID = &pfUtilGetContainingTimelineID;
    p.pfUtility.GetClipName = &pfUtilGetClipName;
    p.pfUtility.EffectWantsCheckedOutFramesToMatchRenderPixelFormat = &pfUtilEffectWantsMatchingFormat;
    // The v4 table is a prefix of every later one; the effect only uses
    // GetContainingTimelineID / GetFilterInstanceID, both v4 members, so one
    // struct answers v4 upward.
    for (int v = kPFUtilitySuiteVersion4; v <= kPFUtilitySuiteVersion; ++v) {
        p.registerSuite(kPFUtilitySuite, v, &p.pfUtility);
    }

    // The Source Settings Suite.  v2 is a typedef of v1, so one table serves
    // both versions - which also means the effect's "acquire v2, fall back to
    // v1" logic is exercised against a host that really does offer both.
    p.pfSourceSettings.SetIsSourceSettingsEffect = &pfSourceSettingsSetIsSourceSettingsEffect;
    p.pfSourceSettings.PerformSourceSettingsCommand = &pfSourceSettingsPerformCommand;
    p.registerSuite(kPFSourceSettingsSuite, kPFSourceSettingsSuiteVersion1, &p.pfSourceSettings);
    p.registerSuite(kPFSourceSettingsSuite, kPFSourceSettingsSuiteVersion2, &p.pfSourceSettings);

    // The Param Utils Suite v3: PF_UpdateParamUI recorded, the rest defined
    // refusals (see the section above).
    p.pfParamUtils.PF_UpdateParamUI = &pfParamUtilsUpdateParamUI;
    p.pfParamUtils.PF_GetCurrentState = &pfParamUtilsGetCurrentState;
    p.pfParamUtils.PF_AreStatesIdentical = &pfParamUtilsAreStatesIdentical;
    p.pfParamUtils.PF_IsIdenticalCheckout = &pfParamUtilsIsIdenticalCheckout;
    p.pfParamUtils.PF_FindKeyframeTime = &pfParamUtilsFindKeyframeTime;
    p.pfParamUtils.PF_GetKeyframeCount = &pfParamUtilsGetKeyframeCount;
    p.pfParamUtils.PF_CheckoutKeyframe = &pfParamUtilsCheckoutKeyframe;
    p.pfParamUtils.PF_CheckinKeyframe = &pfParamUtilsCheckinKeyframe;
    p.pfParamUtils.PF_KeyIndexToTime = &pfParamUtilsKeyIndexToTime;
    p.registerSuite(kPFParamUtilsSuite, kPFParamUtilsSuiteVersion3, &p.pfParamUtils);
}

}  // namespace osv::premiere::mock
