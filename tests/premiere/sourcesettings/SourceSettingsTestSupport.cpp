// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Implementation of the shared scaffolding for the Source Settings tests.

#include "SourceSettingsTestSupport.h"

#include "SourceSettingsParams.h"

#include <cstring>

namespace osv::premiere::sourcesettings::test {

namespace {

/// The module is staged in the build's plugins/OpenOSV directory, whose path
/// the build passes in as OSV_SOURCE_SETTINGS_AEX_PATH.  Falling back to a
/// bare name covers the case where a developer copied the module next to the
/// tests by hand.
std::wstring aexPath() {
#ifdef OSV_SOURCE_SETTINGS_AEX_PATH
    return std::wstring(OSV_SOURCE_SETTINGS_AEX_PATH);
#else
    return L"OpenOSVSourceSettings.aex";
#endif
}

/// Human-readable GetLastError().
std::string lastErrorText(DWORD code) {
    char* buffer = nullptr;
    const DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code,
        0, reinterpret_cast<char*>(&buffer), 0, nullptr);
    std::string text = (len && buffer) ? std::string(buffer, len) : std::string("unknown error");
    if (buffer) {
        LocalFree(buffer);
    }
    // Trim the trailing CRLF FormatMessage appends.
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n')) {
        text.pop_back();
    }
    return text;
}

}  // namespace

// ---------------------------------------------------------------------------
//  LoadedPlugin
// ---------------------------------------------------------------------------

LoadedPlugin::LoadedPlugin() {
    m_path = aexPath();

    // LOAD_WITH_ALTERED_SEARCH_PATH makes the module's own folder the first
    // place the loader looks for ITS dependencies, which is exactly how
    // Premiere loads a plug-in out of MediaCore - and it is what lets the
    // staged fmt.dll / spdlog.dll beside the .aex be found.
    m_module = LoadLibraryExW(m_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m_module) {
        const DWORD err = GetLastError();
        m_error = "LoadLibraryExW failed (" + std::to_string(err) + "): " + lastErrorText(err);
        return;
    }

    // GetProcAddress by the exact name the PiPL's CodeWin64X86 property
    // declares.  A mismatch here is the single most common way a plug-in
    // silently does nothing in a real host.
    m_effectMain =
        reinterpret_cast<EffectMainFn>(reinterpret_cast<void*>(GetProcAddress(m_module, "EffectMain")));
    if (!m_effectMain) {
        m_error = "the module exports no 'EffectMain' (the name the PiPL's CodeWin64X86 property declares)";
    }
}

LoadedPlugin& LoadedPlugin::instance() {
    static LoadedPlugin plugin;
    return plugin;
}

// ---------------------------------------------------------------------------
//  EffectFixture
// ---------------------------------------------------------------------------

EffectFixture::EffectFixture(A_long applId) {
    // Timeline id 1 and filter instance 1: the values do not matter to this
    // effect (it reads no geometry), but a zero timeline would make the mock
    // report "no sequence", which is a state worth NOT having by accident.
    m_ref = m_host.createEffectRef(1, 1);

    mock::InDataSpec spec;
    m_inData = m_host.makeInData(m_ref, spec);
    m_inData.appl_id = applId;

    LoadedPlugin& plugin = LoadedPlugin::instance();
    if (!plugin.ok()) {
        return;  // every accessor then reports the zeroed defaults
    }

    // The order matters and is the host's: GLOBAL_SETUP first (the module
    // declares itself and its flags), then PARAMS_SETUP (the parameter list
    // is created).  Reversing them would mean reading parameters from a
    // module that had not yet been set up.
    m_globalOut = m_host.makeOutData();
    m_globalErr = plugin.effectMain()(PF_Cmd_GLOBAL_SETUP, &m_inData, &m_globalOut, nullptr, nullptr, nullptr);

    m_paramsOut = m_host.makeOutData();
    m_paramsErr = plugin.effectMain()(PF_Cmd_PARAMS_SETUP, &m_inData, &m_paramsOut, nullptr, nullptr, nullptr);
}

EffectFixture::~EffectFixture() {
    LoadedPlugin& plugin = LoadedPlugin::instance();
    if (plugin.ok() && m_ref) {
        // GLOBAL_SETDOWN closes the log.  Sending it per fixture is what
        // Premiere does when it unloads a plug-in, and it keeps the log file
        // from being held open across the whole run.
        PF_OutData out = m_host.makeOutData();
        (void)plugin.effectMain()(PF_Cmd_GLOBAL_SETDOWN, &m_inData, &out, nullptr, nullptr, nullptr);
    }
    if (m_ref) {
        m_host.destroyEffectRef(m_ref);
    }
}

PF_Err EffectFixture::send(PF_Cmd cmd, void* extra, PF_OutData* outData) {
    LoadedPlugin& plugin = LoadedPlugin::instance();
    if (!plugin.ok()) {
        return PF_Err_INTERNAL_STRUCT_DAMAGED;
    }
    PF_OutData local = m_host.makeOutData();
    PF_OutData* out = outData ? outData : &local;
    std::vector<PF_ParamDef*> array = params();
    return plugin.effectMain()(cmd, &m_inData, out, array.empty() ? nullptr : array.data(), nullptr, extra);
}

std::vector<PF_ParamDef*> EffectFixture::params() { return m_host.renderParams(m_ref); }

void EffectFixture::setPopup(int aeIndex, int value) {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return;
    }
    PF_ParamDef def = added[static_cast<std::size_t>(aeIndex) - 1u];
    def.u.pd.value = static_cast<A_long>(value);
    m_host.setParamValue(m_ref, aeIndex, def);
}

void EffectFixture::setCheckbox(int aeIndex, bool value) {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return;
    }
    PF_ParamDef def = added[static_cast<std::size_t>(aeIndex) - 1u];
    def.u.bd.value = value ? 1 : 0;
    m_host.setParamValue(m_ref, aeIndex, def);
}

void EffectFixture::setSlider(int aeIndex, double value) {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return;
    }
    PF_ParamDef def = added[static_cast<std::size_t>(aeIndex) - 1u];
    def.u.fs_d.value = static_cast<PF_FpShort>(value);
    m_host.setParamValue(m_ref, aeIndex, def);
}

int EffectFixture::popup(int aeIndex) const {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return 0;
    }
    return static_cast<int>(added[static_cast<std::size_t>(aeIndex) - 1u].u.pd.value);
}

bool EffectFixture::checkbox(int aeIndex) const {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return false;
    }
    return added[static_cast<std::size_t>(aeIndex) - 1u].u.bd.value != 0;
}

double EffectFixture::slider(int aeIndex) const {
    const std::vector<PF_ParamDef> added = m_host.addedParams(m_ref);
    if (aeIndex < 1 || static_cast<std::size_t>(aeIndex) > added.size()) {
        return 0.0;
    }
    return static_cast<double>(added[static_cast<std::size_t>(aeIndex) - 1u].u.fs_d.value);
}

// ---------------------------------------------------------------------------
//  PiPL
// ---------------------------------------------------------------------------

std::uint32_t PiplProperty::asUint32(std::size_t offset) const noexcept {
    if (data.size() < offset + 4u) {
        return 0u;
    }
    std::uint32_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::uint16_t PiplProperty::asUint16(std::size_t offset) const noexcept {
    if (data.size() < offset + 2u) {
        return 0u;
    }
    std::uint16_t value = 0;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

std::string PiplProperty::asPascalString() const {
    if (data.empty()) {
        return {};
    }
    const std::size_t length = data[0];
    if (length + 1u > data.size()) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(data.data() + 1), length);
}

std::string PiplProperty::asCString() const {
    if (data.empty()) {
        return {};
    }
    const char* begin = reinterpret_cast<const char*>(data.data());
    const std::size_t length = ::strnlen(begin, data.size());
    return std::string(begin, length);
}

std::vector<PiplProperty> readPipl(HMODULE module, std::string* error) {
    std::vector<PiplProperty> properties;
    auto fail = [&](const char* message) {
        if (error) {
            *error = message;
        }
        return std::vector<PiplProperty>{};
    };

    // Premiere looks the resource up by the numeric type 'PiPL' and id 16000,
    // exactly as here.
    HRSRC found = FindResourceW(module, MAKEINTRESOURCEW(16000), L"PiPL");
    if (!found) {
        return fail("FindResourceW(16000, 'PiPL') found nothing");
    }
    const DWORD size = SizeofResource(module, found);
    HGLOBAL loaded = LoadResource(module, found);
    if (!loaded || size < 10u) {
        return fail("the PiPL resource could not be loaded or is too small");
    }
    const auto* bytes = static_cast<const std::uint8_t*>(LockResource(loaded));
    if (!bytes) {
        return fail("LockResource returned null");
    }

    std::size_t offset = 0;
    auto readUint16 = [&](std::uint16_t& out) {
        if (offset + 2u > size) {
            return false;
        }
        std::memcpy(&out, bytes + offset, 2u);
        offset += 2u;
        return true;
    };
    auto readUint32 = [&](std::uint32_t& out) {
        if (offset + 4u > size) {
            return false;
        }
        std::memcpy(&out, bytes + offset, 4u);
        offset += 4u;
        return true;
    };

    std::uint16_t version = 0;
    std::uint32_t reserved = 0;
    std::uint32_t count = 0;
    if (!readUint16(version) || !readUint32(reserved) || !readUint32(count)) {
        return fail("the PiPL header is truncated");
    }
    if (version != 1u) {
        return fail("unexpected PiPL structure version");
    }
    if (count == 0u || count > 64u) {
        return fail("implausible PiPL property count");
    }

    for (std::uint32_t i = 0; i < count; ++i) {
        if (offset + 16u > size) {
            return fail("a PiPL property header is truncated");
        }
        // The vendor and key four-character codes are stored byte-reversed.
        char vendor[5] = {};
        char key[5] = {};
        for (int c = 0; c < 4; ++c) {
            vendor[c] = static_cast<char>(bytes[offset + 3u - static_cast<std::size_t>(c)]);
            key[c] = static_cast<char>(bytes[offset + 4u + 3u - static_cast<std::size_t>(c)]);
        }
        offset += 8u;
        std::uint32_t id = 0;
        std::uint32_t length = 0;
        if (!readUint32(id) || !readUint32(length)) {
            return fail("a PiPL property length is truncated");
        }
        if (std::string(vendor) != "8BIM") {
            return fail("a PiPL property is not vendor 8BIM");
        }
        if (offset + length > size) {
            return fail("a PiPL property payload runs past the resource");
        }

        PiplProperty property;
        property.key = key;
        property.data.assign(bytes + offset, bytes + offset + length);
        properties.push_back(std::move(property));

        // Payloads are padded up to a multiple of four bytes.  Only an
        // odd-length property makes this visible: AE_Effect_Info_Flags is
        // emitted with length 2 and the next property still begins four bytes
        // later, so skipping only `length` desynchronises the reader.
        offset += (length + 3u) & ~static_cast<std::size_t>(3u);
    }

    if (error) {
        error->clear();
    }
    return properties;
}

const PiplProperty* findProperty(const std::vector<PiplProperty>& properties, const char* key) {
    if (!key) {
        return nullptr;
    }
    for (const PiplProperty& p : properties) {
        if (p.key == key) {
            return &p;
        }
    }
    return nullptr;
}

std::string toUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                                           nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), needed, nullptr,
                        nullptr);
    return out;
}

}  // namespace osv::premiere::sourcesettings::test
