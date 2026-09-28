// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Settings.cpp - defaults, sanitising, JSON and the settings file (see
// Settings.h).

#include "Settings.h"

#include "Queue.h"  // pathToUtf8 / pathFromUtf8

#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace osvgui {

namespace fs = std::filesystem;

/// Keys stay in the order they are written, so the file reads top to bottom
/// the way the window is laid out.
using Json = nlohmann::ordered_json;

namespace {

// ---------------------------------------------------------------------------
//  Limits
// ---------------------------------------------------------------------------

/// Longest string field kept (a path, a pattern, the extra arguments).
constexpr std::size_t kMaxText = 4096;
/// Largest settings file read: a real one is about 2 KB.
constexpr std::uintmax_t kMaxFileBytes = 1u << 20;
/// Settings format version written into the file.
constexpr int kFormatVersion = 1;

// ---------------------------------------------------------------------------
//  Sanitising helpers
// ---------------------------------------------------------------------------

/// Replace `token` by `fallback` when it is not one of `choices`.
void keepChoice(std::span<const Choice> choices, std::string& token, const char* fallback) {
    if (choiceIndex(choices, token) < 0) {
        token = fallback;
    }
}

/// Clamp a double into [lo, hi]; NaN / infinity become `fallback`.
[[nodiscard]] double clampFinite(double value, double lo, double hi, double fallback) noexcept {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, lo, hi);
}

/// Cut `text` to `limit` bytes without splitting a UTF-8 sequence, and
/// remove control characters other than tab (a newline in a pattern or a
/// path would break the command line).
void cleanText(std::string& text, std::size_t limit = kMaxText) {
    text.erase(std::remove_if(text.begin(), text.end(),
                              [](char c) {
                                  const auto u = static_cast<unsigned char>(c);
                                  return (u < 0x20 && c != '\t') || u == 0x7F;
                              }),
               text.end());
    if (text.size() > limit) {
        std::size_t cut = limit;
        // Step back over continuation bytes (10xxxxxx) to a character start.
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0u) == 0x80u) {
            --cut;
        }
        text.resize(cut);
    }
}

// ---------------------------------------------------------------------------
//  Typed readers: a key of the wrong type is ignored (the default stays)
// ---------------------------------------------------------------------------

void readString(const Json& obj, const char* key, std::string& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_string()) {
        out = it->get<std::string>();
    }
}

void readBool(const Json& obj, const char* key, bool& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_boolean()) {
        out = it->get<bool>();
    }
}

void readDouble(const Json& obj, const char* key, double& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number()) {
        out = it->get<double>();
    }
}

void readInt(const Json& obj, const char* key, int& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number()) {
        // Through double, so 18.0 and 18 both read, and clamp before the cast
        // so a huge value cannot overflow the int.
        const double v = it->get<double>();
        if (std::isfinite(v)) {
            out = static_cast<int>(std::clamp(v, -2147483648.0, 2147483647.0));
        }
    }
}

void readUint64(const Json& obj, const char* key, std::uint64_t& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number_unsigned()) {
        out = it->get<std::uint64_t>();
    }
}

void readInt64(const Json& obj, const char* key, std::int64_t& out) {
    const auto it = obj.find(key);
    if (it != obj.end() && it->is_number_integer()) {
        out = it->get<std::int64_t>();
    }
}

/// The sub-object `key`, or null when it is missing or not an object.
[[nodiscard]] const Json* section(const Json& root, const char* key) {
    const auto it = root.find(key);
    return (it != root.end() && it->is_object()) ? &*it : nullptr;
}

// ---------------------------------------------------------------------------
//  Environment
// ---------------------------------------------------------------------------

#if defined(_WIN32)
/// A wide environment variable, empty when unset.
[[nodiscard]] std::wstring environmentW(const wchar_t* name) {
    const DWORD needed = ::GetEnvironmentVariableW(name, nullptr, 0);
    if (needed == 0) {
        return {};
    }
    std::wstring value(needed, L'\0');
    const DWORD got = ::GetEnvironmentVariableW(name, value.data(), needed);
    if (got == 0 || got >= needed) {
        return {};
    }
    value.resize(got);
    return value;
}
#else
/// An environment variable, empty when unset.
[[nodiscard]] std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}
#endif

/// A per-process counter so two saves never share a temporary file name.
std::atomic<unsigned> g_saveCounter{0};

}  // namespace

// ===========================================================================
//  Choices
// ===========================================================================

int choiceIndex(std::span<const Choice> choices, std::string_view token) noexcept {
    for (std::size_t i = 0; i < choices.size(); ++i) {
        if (choices[i].token && token == choices[i].token) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

const char* choiceLabel(std::span<const Choice> choices, std::string_view token) noexcept {
    const int index = choiceIndex(choices, token);
    return index >= 0 ? choices[static_cast<std::size_t>(index)].label : "?";
}

// ===========================================================================
//  Sanitise
// ===========================================================================

void sanitise(GuiSettings& s) noexcept {
    try {
        // ---- every choice is one of its list --------------------------------------
        keepChoice(kModes, s.mode, "equirect");
        keepChoice(kEquirectSizes, s.equirectSize, "native");
        keepChoice(kReframeSizes, s.reframeSize, "1920x1080");
        keepChoice(kViews, s.view, "wide");
        keepChoice(kColors, s.color, "auto");
        keepChoice(kTones, s.tone, "aces-bright");
        keepChoice(kLooks, s.look, "dji");
        keepChoice(kStabs, s.stab, "smooth-horizon");
        keepChoice(kCodecs, s.codec, "auto");
        keepChoice(kThemes, s.theme, "dark");

        // ---- numbers in the ranges osvtool and the sliders accept ----------------
        // A rectilinear view reaches 180 degrees only at infinity; 170 is
        // already an extreme wide angle.
        s.fov = clampFinite(s.fov, 20.0, 170.0, 90.0);
        s.yaw = clampFinite(s.yaw, -180.0, 180.0, 0.0);
        s.pitch = clampFinite(s.pitch, -90.0, 90.0, 0.0);
        s.roll = clampFinite(s.roll, -180.0, 180.0, 0.0);
        s.crf = std::clamp(s.crf, 0, 51);

        // ---- text ---------------------------------------------------------------------
        cleanText(s.outputFolder);
        cleanText(s.equirectPattern, 255);
        cleanText(s.reframePattern, 255);
        cleanText(s.ffmpegPath);
        cleanText(s.extraArgs);
        cleanText(s.lastCommandLine, 64 * 1024);
        cleanText(s.encoderCache.ffmpegPath);
        cleanText(s.encoderCache.codec, 64);
        if (s.equirectPattern.empty()) {
            s.equirectPattern = "{name}_360.mp4";
        }
        if (s.reframePattern.empty()) {
            s.reframePattern = "{name}_reframe.mp4";
        }

        // ---- the window: a sane size (it is re-checked against the monitors) ----------
        s.window.width = std::clamp(s.window.width, 640, 16384);
        s.window.height = std::clamp(s.window.height, 480, 16384);
    } catch (...) {
        // Allocation failure while assigning a default: nothing more to do.
    }
}

const std::string& activePattern(const GuiSettings& settings) noexcept {
    return settings.mode == "reframe" ? settings.reframePattern : settings.equirectPattern;
}

// ===========================================================================
//  JSON
// ===========================================================================

std::string settingsToJsonText(const GuiSettings& s) noexcept {
    try {
        Json root;
        root["format"] = "openosv-studio-settings";
        root["version"] = kFormatVersion;

        Json& output = root["output"];
        output["mode"] = s.mode;
        output["equirectSize"] = s.equirectSize;
        output["reframeSize"] = s.reframeSize;
        output["view"] = s.view;
        output["fov"] = s.fov;
        output["yaw"] = s.yaw;
        output["pitch"] = s.pitch;
        output["roll"] = s.roll;

        Json& colour = root["colour"];
        colour["color"] = s.color;
        colour["tone"] = s.tone;
        colour["look"] = s.look;

        Json& motion = root["motion"];
        motion["stab"] = s.stab;
        motion["flare"] = s.flare;
        motion["useUserDefaults"] = s.useUserDefaults;

        Json& encoding = root["encoding"];
        encoding["codec"] = s.codec;
        encoding["crf"] = s.crf;
        encoding["audio"] = s.audio;
        encoding["sphericalMetadata"] = s.sphericalMetadata;

        Json& destination = root["destination"];
        destination["folder"] = s.outputFolder;
        destination["equirectPattern"] = s.equirectPattern;
        destination["reframePattern"] = s.reframePattern;
        destination["skipExisting"] = s.skipExisting;

        Json& tools = root["tools"];
        tools["ffmpeg"] = s.ffmpegPath;
        tools["extraArgs"] = s.extraArgs;

        Json& ui = root["interface"];
        ui["theme"] = s.theme;
        ui["showLog"] = s.showLog;
        ui["batchPreview"] = s.batchPreview;
        ui["advancedOpen"] = s.advancedOpen;
        Json& window = ui["window"];
        window["x"] = s.window.x;
        window["y"] = s.window.y;
        window["width"] = s.window.width;
        window["height"] = s.window.height;
        window["maximized"] = s.window.maximized;

        Json& cache = root["encoderCache"];
        cache["ffmpeg"] = s.encoderCache.ffmpegPath;
        cache["size"] = s.encoderCache.ffmpegSize;
        cache["time"] = s.encoderCache.ffmpegTime;
        cache["codec"] = s.encoderCache.codec;

        root["lastCommandLine"] = s.lastCommandLine;

        // `replace`: a string that is not valid UTF-8 (a hand-edited file)
        // is written with U+FFFD instead of throwing.
        return root.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
    } catch (...) {
        return {};
    }
}

bool settingsFromJsonText(std::string_view text, GuiSettings& out, std::string* error) noexcept {
    try {
        // No exceptions from the parser, comments allowed (a hand-edited
        // file may carry them).
        const Json root = Json::parse(text.begin(), text.end(), nullptr, false, true);
        if (root.is_discarded() || !root.is_object()) {
            if (error) {
                *error = "not a JSON object";
            }
            sanitise(out);
            return false;
        }

        if (const Json* o = section(root, "output")) {
            readString(*o, "mode", out.mode);
            readString(*o, "equirectSize", out.equirectSize);
            readString(*o, "reframeSize", out.reframeSize);
            readString(*o, "view", out.view);
            readDouble(*o, "fov", out.fov);
            readDouble(*o, "yaw", out.yaw);
            readDouble(*o, "pitch", out.pitch);
            readDouble(*o, "roll", out.roll);
        }
        if (const Json* c = section(root, "colour")) {
            readString(*c, "color", out.color);
            readString(*c, "tone", out.tone);
            readString(*c, "look", out.look);
        }
        if (const Json* m = section(root, "motion")) {
            readString(*m, "stab", out.stab);
            readBool(*m, "flare", out.flare);
            readBool(*m, "useUserDefaults", out.useUserDefaults);
        }
        if (const Json* e = section(root, "encoding")) {
            readString(*e, "codec", out.codec);
            readInt(*e, "crf", out.crf);
            readBool(*e, "audio", out.audio);
            readBool(*e, "sphericalMetadata", out.sphericalMetadata);
        }
        if (const Json* d = section(root, "destination")) {
            readString(*d, "folder", out.outputFolder);
            readString(*d, "equirectPattern", out.equirectPattern);
            readString(*d, "reframePattern", out.reframePattern);
            readBool(*d, "skipExisting", out.skipExisting);
        }
        if (const Json* t = section(root, "tools")) {
            readString(*t, "ffmpeg", out.ffmpegPath);
            readString(*t, "extraArgs", out.extraArgs);
        }
        if (const Json* ui = section(root, "interface")) {
            readString(*ui, "theme", out.theme);
            readBool(*ui, "showLog", out.showLog);
            readBool(*ui, "batchPreview", out.batchPreview);
            readBool(*ui, "advancedOpen", out.advancedOpen);
            if (const Json* w = section(*ui, "window")) {
                readInt(*w, "x", out.window.x);
                readInt(*w, "y", out.window.y);
                readInt(*w, "width", out.window.width);
                readInt(*w, "height", out.window.height);
                readBool(*w, "maximized", out.window.maximized);
            }
        }
        if (const Json* cache = section(root, "encoderCache")) {
            readString(*cache, "ffmpeg", out.encoderCache.ffmpegPath);
            readUint64(*cache, "size", out.encoderCache.ffmpegSize);
            readInt64(*cache, "time", out.encoderCache.ffmpegTime);
            readString(*cache, "codec", out.encoderCache.codec);
        }
        readString(root, "lastCommandLine", out.lastCommandLine);

        sanitise(out);
        return true;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
    } catch (...) {
        if (error) {
            *error = "unexpected error";
        }
    }
    sanitise(out);
    return false;
}

// ===========================================================================
//  The file
// ===========================================================================

fs::path defaultSettingsPath() noexcept {
    try {
#if defined(_WIN32)
        // The explicit override first (tests, a portable install).
        const std::wstring overridePath = environmentW(L"OPENOSV_GUI_SETTINGS");
        if (!overridePath.empty()) {
            return fs::path(overridePath);
        }
        // %APPDATA% (roaming), beside the Premiere plug-ins' defaults.json.
        const std::wstring appData = environmentW(L"APPDATA");
        if (!appData.empty()) {
            return fs::path(appData) / L"OpenOSV" / L"osvgui.json";
        }
#else
        const std::string overridePath = environment("OPENOSV_GUI_SETTINGS");
        if (!overridePath.empty()) {
            return fs::path(overridePath);
        }
#if defined(__APPLE__)
        // Application Support, where the plug-ins keep their defaults too.
        const std::string macHome = environment("HOME");
        if (!macHome.empty()) {
            return fs::path(macHome) / "Library" / "Application Support" / "OpenOSV" / "osvgui.json";
        }
#endif
        const std::string xdg = environment("XDG_CONFIG_HOME");
        if (!xdg.empty()) {
            return fs::path(xdg) / "openosv" / "osvgui.json";
        }
        const std::string home = environment("HOME");
        if (!home.empty()) {
            return fs::path(home) / ".config" / "openosv" / "osvgui.json";
        }
#endif
    } catch (...) {
        // Allocation failure: no location, so nothing is saved.
    }
    return {};
}

bool loadSettings(const fs::path& path, GuiSettings& out, std::string* error) noexcept {
    try {
        if (path.empty()) {
            sanitise(out);
            return true;  // nowhere to read from: first-run defaults
        }
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            sanitise(out);
            return true;  // first run
        }
        const std::uintmax_t size = fs::file_size(path, ec);
        if (ec || size > kMaxFileBytes) {
            if (error) {
                *error = ec ? ec.message() : std::string("the file is too large to be a settings file");
            }
            sanitise(out);
            return false;
        }
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            if (error) {
                *error = "cannot open " + pathToUtf8(path);
            }
            sanitise(out);
            return false;
        }
        std::string text(static_cast<std::size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(0, in.gcount())));
        return settingsFromJsonText(text, out, error);
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
    } catch (...) {
        if (error) {
            *error = "unexpected error";
        }
    }
    sanitise(out);
    return false;
}

bool saveSettings(const fs::path& path, const GuiSettings& settings, std::string* error) noexcept {
    try {
        if (path.empty()) {
            if (error) {
                *error = "no settings location (neither OPENOSV_GUI_SETTINGS nor the user folder is set)";
            }
            return false;
        }
        const std::string text = settingsToJsonText(settings);
        if (text.empty()) {
            if (error) {
                *error = "out of memory";
            }
            return false;
        }
        std::error_code ec;
        if (path.has_parent_path()) {
            fs::create_directories(path.parent_path(), ec);  // ec: reported by the open below
        }

        // ---- write a sibling, flushed, then rename it over the old file -----------
#if defined(_WIN32)
        const unsigned long pid = static_cast<unsigned long>(::GetCurrentProcessId());
#else
        const unsigned long pid = static_cast<unsigned long>(::getpid());
#endif
        fs::path temp = path;
        temp += ".tmp-" + std::to_string(pid) + "-" + std::to_string(g_saveCounter.fetch_add(1));
        {
            std::ofstream outFile(temp, std::ios::binary | std::ios::trunc);
            if (!outFile) {
                if (error) {
                    *error = "cannot write " + pathToUtf8(temp);
                }
                return false;
            }
            outFile.write(text.data(), static_cast<std::streamsize>(text.size()));
            outFile.flush();
            if (!outFile) {
                outFile.close();
                fs::remove(temp, ec);
                if (error) {
                    *error = "cannot write " + pathToUtf8(temp);
                }
                return false;
            }
        }
        // A reader holding the old file without delete sharing (a virus
        // scanner, an editor) makes the rename fail for a moment: retry.
        for (int attempt = 0; attempt < 10; ++attempt) {
            fs::rename(temp, path, ec);
            if (!ec) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        std::error_code ignored;
        fs::remove(temp, ignored);
        if (error) {
            *error = "cannot replace " + pathToUtf8(path) + ": " + ec.message();
        }
        return false;
    } catch (const std::exception& e) {
        if (error) {
            *error = e.what();
        }
    } catch (...) {
        if (error) {
            *error = "unexpected error";
        }
    }
    return false;
}

}  // namespace osvgui
