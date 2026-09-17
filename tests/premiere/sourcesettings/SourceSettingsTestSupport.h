// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Shared scaffolding for the OpenOSVSourceSettings.aex tests.
//
// The tests deliberately load the BUILT module with LoadLibraryW rather than
// linking its objects: that is the only way to prove what Premiere actually
// sees - the exported symbol name, the PiPL resource, the flag words at run
// time and the behaviour of the module as one linked unit, including its
// static initialisers and its delay-load hook.
//
// Mirrors tests/premiere/reframe/ReframeTestSupport.h, minus everything about
// pixels: this effect renders nothing, so there is no synthetic imagery and
// no parity comparison here.  What there is instead is a PiPL reader (the
// match name is the entire binding to the importer, so it is checked byte for
// byte) and a small harness that drives the module through the mock host.
#pragma once

#include "MockHost.h"

#include "AEConfig.h"
#include "A.h"
#include "AE_Effect.h"

#include <string>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace osv::premiere::sourcesettings::test {

/// Signature of the AE entry point, exactly as the PiPL names it.
using EffectMainFn = PF_Err (*)(PF_Cmd cmd, PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                PF_LayerDef* output, void* extra);

/// The loaded plug-in.
///
/// One instance is shared by the whole test run: the module keeps
/// process-wide state (the log file) and loading and unloading it per test
/// case would exercise a path Premiere never takes.
class LoadedPlugin {
public:
    /// The singleton, loaded on first use.  Never null; check `ok()`.
    static LoadedPlugin& instance();

    [[nodiscard]] bool ok() const noexcept { return m_module != nullptr && m_effectMain != nullptr; }
    /// Why ok() is false ("" when it is true).
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }

    [[nodiscard]] HMODULE module() const noexcept { return m_module; }
    [[nodiscard]] EffectMainFn effectMain() const noexcept { return m_effectMain; }
    /// Full path the module was loaded from.
    [[nodiscard]] const std::wstring& path() const noexcept { return m_path; }

private:
    LoadedPlugin();
    ~LoadedPlugin() = default;
    LoadedPlugin(const LoadedPlugin&) = delete;
    LoadedPlugin& operator=(const LoadedPlugin&) = delete;

    HMODULE m_module = nullptr;
    EffectMainFn m_effectMain = nullptr;
    std::wstring m_path;
    std::string m_error;
};

// ---------------------------------------------------------------------------
//  Driving the effect
// ---------------------------------------------------------------------------

/// A mock host plus one effect instance, set up the way Premiere sets one up.
///
/// Every test needs the same four steps - create a host, create an effect
/// ref, run GLOBAL_SETUP so the module registers itself, run PARAMS_SETUP so
/// the parameter list exists - and getting any of them in the wrong order
/// produces a confusing failure rather than a clear one.  So they happen in
/// the constructor, in that order, and the results are inspectable.
class EffectFixture {
public:
    /// `applId` is what the host reports as PF_InData::appl_id.  The default
    /// is Premiere; a test passes 'FXTC' to prove the After Effects path
    /// registers nothing.
    explicit EffectFixture(A_long applId = 'PrMr');
    ~EffectFixture();

    EffectFixture(const EffectFixture&) = delete;
    EffectFixture& operator=(const EffectFixture&) = delete;

    [[nodiscard]] mock::MockHost& host() noexcept { return m_host; }
    [[nodiscard]] PF_ProgPtr ref() const noexcept { return m_ref; }

    /// out_data as GLOBAL_SETUP left it, so a test can read the flag words.
    [[nodiscard]] const PF_OutData& globalSetupOut() const noexcept { return m_globalOut; }
    /// out_data as PARAMS_SETUP left it, for num_params.
    [[nodiscard]] const PF_OutData& paramsSetupOut() const noexcept { return m_paramsOut; }
    [[nodiscard]] PF_Err globalSetupErr() const noexcept { return m_globalErr; }
    [[nodiscard]] PF_Err paramsSetupErr() const noexcept { return m_paramsErr; }

    /// Send an arbitrary selector with the fixture's in_data, a fresh
    /// out_data and the current parameter array.  `extra` is passed through.
    /// The out_data is returned by reference so a handler's writes are
    /// visible.
    PF_Err send(PF_Cmd cmd, void* extra, PF_OutData* outData = nullptr);

    /// The parameter array as PF_Cmd_RENDER would receive it: [0] the input
    /// layer, [1..n] the added controls.  Writes through it are visible to
    /// the mock host afterwards, which is how a test reads back the values
    /// SEQUENCE_SETUP committed.
    [[nodiscard]] std::vector<PF_ParamDef*> params();

    /// Set one popup control's value (1-based, as AE stores it).
    void setPopup(int aeIndex, int value);
    /// Set one checkbox control's value.
    void setCheckbox(int aeIndex, bool value);
    /// Set one float slider's value.
    void setSlider(int aeIndex, double value);

    /// Read one control back.
    [[nodiscard]] int popup(int aeIndex) const;
    [[nodiscard]] bool checkbox(int aeIndex) const;
    [[nodiscard]] double slider(int aeIndex) const;

private:
    mock::MockHost m_host;
    PF_ProgPtr m_ref = nullptr;
    PF_InData m_inData{};
    PF_OutData m_globalOut{};
    PF_OutData m_paramsOut{};
    PF_Err m_globalErr = PF_Err_NONE;
    PF_Err m_paramsErr = PF_Err_NONE;
};

// ---------------------------------------------------------------------------
//  The PiPL
// ---------------------------------------------------------------------------

/// One parsed PiPL property.
struct PiplProperty {
    std::string key;                 ///< Four characters, un-reversed ("eFKT").
    std::vector<std::uint8_t> data;  ///< Raw payload.

    /// The payload as a 32-bit little-endian word (0 when too short).
    [[nodiscard]] std::uint32_t asUint32(std::size_t offset = 0) const noexcept;
    /// The payload as a 16-bit little-endian word (0 when too short).
    [[nodiscard]] std::uint16_t asUint16(std::size_t offset = 0) const noexcept;
    /// The payload as a Pascal string (length byte then characters), which is
    /// how Name, Category and Match Name are stored.
    [[nodiscard]] std::string asPascalString() const;
    /// The payload as a NUL-terminated C string (how CodeWin64X86 stores the
    /// entry point name).
    [[nodiscard]] std::string asCString() const;
};

/// Read resource 16000 of type 'PiPL' out of `module` and parse it, keyed by
/// property code.  Returns an empty map on failure and writes why into
/// `error`.
///
/// The layout is the one PiPLtool actually emits:
///
///     uint16  version    (1)            <- 16-bit, NOT 32
///     uint32  reserved   (0)
///     uint32  count
///     then `count` properties, each:
///         char[4] vendor   ("8BIM", stored byte-reversed)
///         char[4] key      (e.g. "eFKT", stored byte-reversed)
///         uint32  id       (0)
///         uint32  length
///         byte[length] data, padded up to a multiple of four
///
/// The 16-bit version word makes the header TEN bytes, so nothing after it is
/// four-byte aligned - which is why every word is memcpy'd out rather than
/// read through a cast pointer.
[[nodiscard]] std::vector<PiplProperty> readPipl(HMODULE module, std::string* error);

/// One property by key, or nullptr when absent.
[[nodiscard]] const PiplProperty* findProperty(const std::vector<PiplProperty>& properties, const char* key);

/// UTF-16 -> UTF-8, for putting a module path into a Catch2 INFO message.
/// Narrowing each wchar_t with a char cast would mangle any non-ASCII
/// character in the build directory's name.
[[nodiscard]] std::string toUtf8(const std::wstring& text);

}  // namespace osv::premiere::sourcesettings::test
