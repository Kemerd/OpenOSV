// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Settings.h - everything OpenOSV Studio remembers between runs: the render
// options (each one an `osvtool render` option, see CommandBuilder.h), the
// destination, the window, and the last command line it built.
//
// Stored as JSON in the per-user settings folder:
//   Windows  %APPDATA%\OpenOSV\osvgui.json
//   macOS    ~/Library/Application Support/OpenOSV/osvgui.json
//   other    $XDG_CONFIG_HOME/openosv/osvgui.json (or ~/.config/...)
// OPENOSV_GUI_SETTINGS names another file (tests, portable setups).
//
// Reading is forgiving - a missing key keeps its default, a value that is
// not one of the known choices is replaced by the default, numbers are
// clamped - so a hand-edited or older file can never put the UI into a
// state osvtool would reject.  Writing is atomic (a sibling file renamed
// over the old one), so a crash mid-save never leaves half a file.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace osvgui {

// ===========================================================================
//  The choices each option offers
// ===========================================================================

/// One entry of a choice list: the token osvtool (or the settings file)
/// uses, and the label the UI shows.
struct Choice {
    const char* token;  ///< Stable identifier ("equirect", "pq", ...).
    const char* label;  ///< Short UI text, UTF-8.
};

/// Output: the whole sphere (`--mode equirect`) or a flat view (`--mode reframe`).
inline constexpr Choice kModes[] = {{"equirect", "360 equirect"}, {"reframe", "Reframed"}};

/// Equirect sizes.  "native" leaves `--size` out: the clip's own resolution
/// (or, with --use-user-defaults, the Output Size saved in Premiere).
inline constexpr Choice kEquirectSizes[] = {
    {"native", "Native"},
    {"7680x3840", "7680 \xC3\x97 3840  \xC2\xB7  8K"},
    {"5760x2880", "5760 \xC3\x97 2880  \xC2\xB7  6K"},
    {"3840x1920", "3840 \xC3\x97 1920  \xC2\xB7  4K"},
    {"2560x1280", "2560 \xC3\x97 1280"},
    {"1920x960", "1920 \xC3\x97 960"},
};

/// Reframe (flat video) sizes.
inline constexpr Choice kReframeSizes[] = {
    {"3840x2160", "3840 \xC3\x97 2160  \xC2\xB7  4K UHD"},
    {"2560x1440", "2560 \xC3\x97 1440  \xC2\xB7  QHD"},
    {"1920x1080", "1920 \xC3\x97 1080  \xC2\xB7  Full HD"},
    {"1080x1920", "1080 \xC3\x97 1920  \xC2\xB7  Vertical"},
};

/// Reframe views: DJI's presets (`--preset`), or "custom" for an explicit
/// rectilinear field of view (`--fov`).
inline constexpr Choice kViews[] = {
    {"wide", "Wide"},           {"ultra-wide", "Ultra wide"},
    {"dewarping", "Dewarping"}, {"crystal-ball", "Crystal ball"},
    {"asteroid", "Asteroid"},   {"custom", "Custom field of view"},
};

/// Colour output.  "auto" leaves `--color` out: D-Log M clips come out as
/// HDR10 (PQ), SDR recordings stay Rec.709 - osvtool's own rule.
inline constexpr Choice kColors[] = {{"auto", "Auto"}, {"pq", "HDR10"}, {"hlg", "HLG"}, {"709", "SDR"}};

/// HDR transfer function styles (`--tone`, PQ / HLG only).
inline constexpr Choice kTones[] = {
    {"aces-bright", "ACES 2 Bright (outdoor)"}, {"aces-detailed", "ACES 2 Detailed (indoor)"},
    {"bt2408-natural", "BT.2408 Natural"},      {"bt2408-punchy", "BT.2408 Punchy"},
    {"bt2408-neutral", "BT.2408 Neutral"},
};

/// Rec.709 looks (`--look`, SDR only).
inline constexpr Choice kLooks[] = {{"dji", "DJI Studio"}, {"standard", "Standard"}};

/// Stabilisation (`--stab`).  "full" is DJI Studio's direction lock.
inline constexpr Choice kStabs[] = {
    {"off", "Off"},
    {"horizon", "Horizon lock"},
    {"full", "Direction lock"},
    {"smooth", "Smooth"},
    {"smooth-horizon", "Smooth + horizon lock"},
};

/// Video encoders (`--codec`).  "auto" is the one the encoder probe found
/// working on this machine (CommandBuilder.h: pickEncoderOrder).
inline constexpr Choice kCodecs[] = {
    {"auto", "Auto"},
    {"hevc_nvenc", "NVIDIA NVENC"},
    {"hevc_amf", "AMD AMF"},
    {"hevc_qsv", "Intel Quick Sync"},
    {"hevc_videotoolbox", "Apple VideoToolbox"},
    {"libx265", "CPU (x265)"},
};

/// Colour themes of the window itself.
inline constexpr Choice kThemes[] = {{"dark", "Dark"}, {"light", "Light"}};

/// Index of `token` in `choices`, or -1.
[[nodiscard]] int choiceIndex(std::span<const Choice> choices, std::string_view token) noexcept;

/// The label for `token`, or the token itself when it is not in the list.
[[nodiscard]] const char* choiceLabel(std::span<const Choice> choices, std::string_view token) noexcept;

// ===========================================================================
//  The settings
// ===========================================================================

/// Window placement, in GLFW screen coordinates.  x / y of INT32_MIN mean
/// "let the window manager place it".
struct WindowState {
    int x = INT32_MIN;
    int y = INT32_MIN;
    int width = 1240;
    int height = 820;
    bool maximized = false;

    bool operator==(const WindowState&) const = default;
};

/// The encoder the probe picked, and the ffmpeg it was picked for, so the
/// probe (a few test encodes) runs again only when ffmpeg changes.
struct EncoderCache {
    std::string ffmpegPath;        ///< UTF-8 path of the ffmpeg that was probed.
    std::uint64_t ffmpegSize = 0;  ///< Its size in bytes then.
    std::int64_t ffmpegTime = 0;   ///< Its last-write time then (file clock ticks).
    std::string codec;             ///< What it chose ("" = none worked).

    bool operator==(const EncoderCache&) const = default;
};

/// Every setting.  Defaults are the first-run state.
struct GuiSettings {
    // ---- output -----------------------------------------------------------------
    std::string mode = "equirect";          ///< kModes
    std::string equirectSize = "native";    ///< kEquirectSizes
    std::string reframeSize = "1920x1080";  ///< kReframeSizes
    std::string view = "wide";              ///< kViews
    double fov = 90.0;                      ///< Horizontal FOV, degrees (view "custom").
    double yaw = 0.0;                       ///< Pan, degrees (-180..180).
    double pitch = 0.0;                     ///< Tilt, degrees (-90..90).
    double roll = 0.0;                      ///< Roll, degrees (-180..180).

    // ---- colour and motion ---------------------------------------------------------
    std::string color = "auto";           ///< kColors
    std::string tone = "aces-bright";     ///< kTones
    std::string look = "dji";             ///< kLooks
    std::string stab = "smooth-horizon";  ///< kStabs (the Source Settings default)
    bool flare = true;                    ///< Sun ghost removal.
    /// Start from the Source Settings saved in Premiere; colour, stabilisation
    /// and sun ghost removal are then left to them (CommandBuilder.h).
    bool useUserDefaults = false;

    // ---- encoding -----------------------------------------------------------------------
    std::string codec = "auto";     ///< kCodecs
    int crf = 18;                   ///< Quality (-crf / -cq), 0..51, lower is better.
    bool audio = true;              ///< Copy the clip's audio into the video.
    bool sphericalMetadata = true;  ///< Tag equirect video as 360 (when osvtool supports it).

    // ---- destination ------------------------------------------------------------------------
    std::string outputFolder;  ///< UTF-8; empty = next to each clip.
    std::string equirectPattern = "{name}_360.mp4";
    std::string reframePattern = "{name}_reframe.mp4";
    bool skipExisting = false;  ///< Leave clips whose output already exists.

    // ---- tools --------------------------------------------------------------------------------
    std::string ffmpegPath;  ///< UTF-8; empty = found on PATH.
    std::string extraArgs;   ///< Appended to every command, verbatim.

    // ---- the window ---------------------------------------------------------------------------
    std::string theme = "dark";  ///< kThemes
    bool showLog = false;        ///< Log pane open.
    bool batchPreview = false;   ///< Preview shows the whole-folder form.
    bool advancedOpen = false;   ///< The Advanced section is expanded.
    WindowState window;

    // ---- remembered facts ----------------------------------------------------------------------
    EncoderCache encoderCache;
    std::string lastCommandLine;  ///< The preview as it was when the settings were saved.

    bool operator==(const GuiSettings&) const = default;
};

/// Bring every field into range: unknown tokens become their defaults,
/// numbers are clamped, over-long strings are cut.  Idempotent.
void sanitise(GuiSettings& settings) noexcept;

/// The file-name pattern in force for the settings' output mode.
[[nodiscard]] const std::string& activePattern(const GuiSettings& settings) noexcept;

// ===========================================================================
//  JSON
// ===========================================================================

/// The settings as pretty-printed JSON text (UTF-8).  Empty only when out
/// of memory.
[[nodiscard]] std::string settingsToJsonText(const GuiSettings& settings) noexcept;

/// Parse settings from JSON text.  Starts from `out` as it is (normally
/// the defaults), takes every recognised key, then sanitises.  False, with
/// `error` set, when the text is not a JSON object; `out` is then left
/// sanitised but otherwise untouched.
bool settingsFromJsonText(std::string_view text, GuiSettings& out, std::string* error = nullptr) noexcept;

// ===========================================================================
//  The file
// ===========================================================================

/// Where the settings live (see the file comment).  Empty when no location
/// can be determined (no APPDATA / HOME at all).
[[nodiscard]] std::filesystem::path defaultSettingsPath() noexcept;

/// Load from `path`.  A missing file is not an error: `out` keeps the
/// defaults and the result is true.  False with `error` for an unreadable
/// or malformed file (the defaults are kept then too).
bool loadSettings(const std::filesystem::path& path, GuiSettings& out, std::string* error = nullptr) noexcept;

/// Save to `path` atomically, creating its folder.  False with `error`.
bool saveSettings(const std::filesystem::path& path, const GuiSettings& settings,
                  std::string* error = nullptr) noexcept;

}  // namespace osvgui
