// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MockOfxHost.h - a small, strict OpenFX host for the tests of OpenOSV.ofx.
//
// ===========================================================================
//  What it is for
// ===========================================================================
// The tests cannot drive DaVinci Resolve or VEGAS Pro, so the OpenFX module
// is proven the way the Premiere plug-ins are: the tests load the bundle this
// build produced and drive it through the C API exactly as a host does -
// OfxSetHost, OfxGetPlugin, Load, Describe, DescribeInContext, CreateInstance,
// InstanceChanged, GetRegionsOfInterest, Render, DestroyInstance, Unload -
// with property sets, parameters and images the tests control.
//
// It is STRICT where a lenient host would hide a bug:
//   * a property read with the wrong type fails (kOfxStatErrValue), as it
//     does in real hosts;
//   * a read of a property that does not exist fails (kOfxStatErrUnknown);
//   * every image handed out is counted, and the tests assert that the
//     plug-in released every one of them.
//
// Parameters keep keyframes (linear between them, held outside), so the
// easing and smoothing paths run against real keyframe queries.
//
// ===========================================================================
//  Which host it plays
// ===========================================================================
// The module classifies its host ONCE, by name, when it is loaded
// (osv::ofx::hostProfile()), and a test process loads it once.  So the host
// the mock plays is chosen per PROCESS, by the environment variable
//
//     OSV_MOCK_OFX_PROFILE = vegas | resolve | (unset: the generic mock)
//
//   * unset   - "OpenOSV.MockOfxHost", which the module treats as it treats
//               any unknown host: exactly like DaVinci Resolve;
//   * resolve - "DaVinciResolveLite", Resolve's own name;
//   * vegas   - "com.vegascreativesoftware.vegas", with VEGAS's host
//               properties: 8-bit and float depths plus its B G R A tokens,
//               its window and app data properties, the instance's VEGAS
//               context, and frame-local generator time.
//
// ctest runs the [vegas] tests (hidden from a plain run) in a process of
// their own with the variable set - see tests/ofx/CMakeLists.txt.
//
// Everything the plug-in can reach from several render threads at once -
// the image counters and the message list - is thread-safe, so tests can
// render cloned instances concurrently the way VEGAS does.
#pragma once

#include "ofxCore.h"
#include "ofxGPURender.h"
#include "ofxImageEffect.h"
#include "ofxMessage.h"
#include "ofxParam.h"
#include "ofxProperty.h"

#include <atomic>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace osv::ofxtest {

// ===========================================================================
//  Property sets
// ===========================================================================

class PropertySet {
public:
    enum class Type { String, Int, Double, Pointer };

    struct Prop {
        Type type = Type::String;
        std::vector<std::string> s;
        std::vector<int> i;
        std::vector<double> d;
        std::vector<void*> p;
        [[nodiscard]] int size() const noexcept;
    };

    [[nodiscard]] OfxPropertySetHandle handle() noexcept { return reinterpret_cast<OfxPropertySetHandle>(this); }
    [[nodiscard]] static PropertySet* from(OfxPropertySetHandle h) noexcept { return reinterpret_cast<PropertySet*>(h); }

    // ---- test-side access (no type checks on write; reads assert the type) --
    void setString(const std::string& name, const std::string& value, int index = 0);
    void setInt(const std::string& name, int value, int index = 0);
    void setDouble(const std::string& name, double value, int index = 0);
    void setPointer(const std::string& name, void* value, int index = 0);
    void setDoubles(const std::string& name, const std::vector<double>& values);
    void setInts(const std::string& name, const std::vector<int>& values);

    [[nodiscard]] bool has(const std::string& name) const;
    [[nodiscard]] int dimension(const std::string& name) const;
    [[nodiscard]] std::string getString(const std::string& name, int index = 0) const;
    [[nodiscard]] int getInt(const std::string& name, int index = 0) const;
    [[nodiscard]] double getDouble(const std::string& name, int index = 0) const;
    [[nodiscard]] void* getPointer(const std::string& name, int index = 0) const;
    [[nodiscard]] std::vector<std::string> getStrings(const std::string& name) const;

    std::map<std::string, Prop> props;
};

// ===========================================================================
//  Parameters
// ===========================================================================

struct Param {
    std::string name;
    std::string type;  ///< kOfxParamType*.
    PropertySet props;

    // Value storage by kind; keys override the static value when present.
    double d = 0.0;
    int i = 0;
    std::string s;
    std::map<double, double> dkeys;
    std::map<double, int> ikeys;

    int pluginWrites = 0;  ///< paramSetValue / paramSetValueAtTime calls made by the plug-in.

    [[nodiscard]] bool isDouble() const noexcept;
    [[nodiscard]] bool isIntLike() const noexcept;  ///< Int, Choice, Boolean.
    [[nodiscard]] bool isString() const noexcept;
    [[nodiscard]] double doubleAt(double time) const;
    [[nodiscard]] int intAt(double time) const;
    [[nodiscard]] std::vector<double> keyTimes() const;
};

class ParamSet {
public:
    [[nodiscard]] OfxParamSetHandle handle() noexcept { return reinterpret_cast<OfxParamSetHandle>(this); }
    [[nodiscard]] static ParamSet* from(OfxParamSetHandle h) noexcept { return reinterpret_cast<ParamSet*>(h); }

    [[nodiscard]] Param* find(const std::string& name);
    [[nodiscard]] const Param* find(const std::string& name) const;
    Param& add(const std::string& name, const std::string& type);

    /// Copy of the descriptor's parameters with values at their defaults.
    void instantiateFrom(const ParamSet& descriptor);

    std::vector<std::unique_ptr<Param>> params;  ///< Definition order.
    PropertySet props;
    int editDepth = 0;
    int editGroups = 0;
};

// ===========================================================================
//  Clips, images, effects
// ===========================================================================

struct Clip {
    std::string name;
    PropertySet props;
    OfxRectD rod{0, 0, 0, 0};  ///< Canonical coordinates.
    /// Fills an image's properties for `time`; false = no image (clipGetImage fails).
    std::function<bool(double time, PropertySet& image)> provide;

    [[nodiscard]] OfxImageClipHandle handle() noexcept { return reinterpret_cast<OfxImageClipHandle>(this); }
    [[nodiscard]] static Clip* from(OfxImageClipHandle h) noexcept { return reinterpret_cast<Clip*>(h); }
};

struct Effect {
    PropertySet props;
    ParamSet params;
    std::map<std::string, std::unique_ptr<Clip>> clips;
    std::vector<std::string> clipOrder;

    [[nodiscard]] OfxImageEffectHandle handle() noexcept { return reinterpret_cast<OfxImageEffectHandle>(this); }
    [[nodiscard]] static Effect* from(OfxImageEffectHandle h) noexcept { return reinterpret_cast<Effect*>(h); }
    [[nodiscard]] Clip* clip(const std::string& name);
};

// ===========================================================================
//  The host
// ===========================================================================

class MockHost {
public:
    /// The host families the mock can play (see "Which host it plays").
    enum class Profile { Generic, Resolve, Vegas };

    /// The process-wide host (the suites are plain C functions).
    static MockHost& instance();

    [[nodiscard]] OfxHost* ofxHost() noexcept { return &m_host; }

    /// The host this process plays, from OSV_MOCK_OFX_PROFILE.
    [[nodiscard]] Profile profile() const noexcept { return m_profile; }
    /// True when this process plays VEGAS Pro.
    [[nodiscard]] bool isVegas() const noexcept { return m_profile == Profile::Vegas; }
    /// "generic", "resolve" or "vegas".
    [[nodiscard]] static const char* profileName(Profile profile) noexcept;

    /// kOfxPropName, the supported depths and the rest of what the host
    /// says about itself (filled for the profile at construction).
    PropertySet hostProps;
    /// Everything posted through the message suite.  The suite appends under
    /// `messagesMutex`; tests read it between renders.
    std::vector<std::string> messages;
    std::mutex messagesMutex;
    std::atomic<int> imagesOut{0};    ///< Images handed out and not yet released.
    std::atomic<int> imagesTotal{0};  ///< Images handed out in total.

private:
    MockHost();
    OfxHost m_host{};
    Profile m_profile = Profile::Generic;
};

// ===========================================================================
//  The loaded module
// ===========================================================================

class LoadedModule {
public:
    /// Load the module at `path` (LoadLibraryExW on Windows, dlopen on
    /// macOS - the bundle's Contents/MacOS binary) and hand it the mock host.
    explicit LoadedModule(const std::filesystem::path& path);
    ~LoadedModule();
    LoadedModule(const LoadedModule&) = delete;
    LoadedModule& operator=(const LoadedModule&) = delete;

    [[nodiscard]] bool ok() const noexcept { return m_module && m_getCount && m_getPlugin; }
    [[nodiscard]] int pluginCount() const;
    [[nodiscard]] OfxPlugin* plugin(int index) const;
    [[nodiscard]] OfxPlugin* plugin(const std::string& identifier) const;
    [[nodiscard]] bool hasSetHostExport() const noexcept { return m_setHost != nullptr; }

private:
    void* m_module = nullptr;  ///< HMODULE on Windows, a dlopen handle elsewhere.
    int (*m_getCount)() = nullptr;
    OfxPlugin* (*m_getPlugin)(int) = nullptr;
    OfxStatus (*m_setHost)(const OfxHost*) = nullptr;
};

/// Drives one plug-in through its actions.
class PluginHarness {
public:
    explicit PluginHarness(OfxPlugin* plugin);

    [[nodiscard]] OfxStatus action(const char* name, void* handle, PropertySet* inArgs, PropertySet* outArgs);

    OfxStatus load();
    OfxStatus unload();
    /// kOfxActionDescribe on a fresh descriptor (kept in `descriptor`).
    OfxStatus describe();
    /// kOfxImageEffectActionDescribeInContext on a fresh context descriptor.
    std::unique_ptr<Effect> describeInContext(const std::string& context, OfxStatus* status = nullptr);

    /// A new instance for `context`: the context descriptor's clips and
    /// parameters, project properties for a `projectW` x `projectH` project
    /// at `fps`, then kOfxActionCreateInstance.
    ///
    /// `pixelAspect` is the project's pixel aspect ratio
    /// (kOfxImageEffectPropProjectPixelAspectRatio).  `projectW` stays the
    /// width in PIXELS; the project size, its extent and every clip's RoD
    /// are canonical, i.e. projectW * pixelAspect wide, as a host with
    /// non-square pixels states them (an HDV project: 1440 pixels, 1920
    /// canonical).  The default 1.0 sets exactly what it always did.
    std::unique_ptr<Effect> createInstance(const std::string& context, int projectW, int projectH, double fps,
                                           OfxStatus* status = nullptr, double pixelAspect = 1.0);
    OfxStatus destroyInstance(Effect& effect);

    OfxStatus instanceChanged(Effect& effect, const std::string& param, const std::string& reason, double time);

    /// kOfxActionInstanceChanged for a CLIP (kOfxPropType = kOfxTypeClip):
    /// what VEGAS sends for its clip "Output" alongside parameter changes.
    OfxStatus instanceChangedClip(Effect& effect, const std::string& clip, const std::string& reason, double time);

    /// kOfxImageEffectActionGetClipPreferences on `effect`, with `prefs`
    /// pre-filled by the caller the way a host fills them.
    OfxStatus clipPreferences(Effect& effect, PropertySet& prefs);

    struct RenderArgs {
        double time = 0.0;  ///< Frames; a field render passes x.5 for the second field.
        OfxRectI window{0, 0, 0, 0};
        double scaleX = 1.0;
        double scaleY = 1.0;
        bool cuda = false;
        void* stream = nullptr;
        bool setCudaProps = false;  ///< Only a CUDA-capable host sets the CUDA inArgs at all.
        bool interactive = false;
        bool draft = false;
        std::string field = kOfxImageFieldNone;  ///< kOfxImageEffectPropFieldToRender.
        /// VEGAS's "OfxImageEffectPropRenderQuality" (ofxSonyVegas.h), e.g.
        /// "OfxImageEffectPropRenderQualityPreview"; empty = not set at all,
        /// as every host but VEGAS leaves it.
        std::string quality;
    };
    OfxStatus render(Effect& effect, const RenderArgs& args);

    /// kOfxImageEffectActionGetRegionsOfInterest: the RoI the plug-in asks of
    /// clip `clip` for an output region `region`.
    OfxStatus regionsOfInterest(Effect& effect, double time, const OfxRectD& region, const std::string& clip,
                                OfxRectD& roi);

    Effect descriptor;

private:
    OfxPlugin* m_plugin = nullptr;
};

}  // namespace osv::ofxtest
