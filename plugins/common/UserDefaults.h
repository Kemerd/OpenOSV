// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// UserDefaults: the Source Settings a user wants every NEW clip to start with.
//
// ===========================================================================
//  WHAT IT IS
// ===========================================================================
//
// PrefsBlob::defaults() is the built-in answer to "what does a clip look like
// before anybody touched it" - PQ, smooth + horizon lock, seam search on, the
// sky seam fix on, and so on.  It is compiled in, it is what every test pins,
// and it never changes behind anybody's back.
//
// Users have opinions, though: a Rec.709 editor wants Rec.709, somebody who
// grades in log wants the D-Log M passthrough, a laptop user wants 2560 x 1280.
// Setting that on every clip by hand is the kind of chore that makes a tool
// feel hostile.  So the Source Settings effect ("Save as Default for New
// Clips") and the modal dialog ("Save as Default") can store the settings of
// the clip in front of the user as THEIR defaults, and every newly imported
// clip starts from those instead of the built-in ones.  Clips that already
// have stored settings keep them: this file only decides where a clip STARTS.
//
// ===========================================================================
//  THE FILE
// ===========================================================================
//
// %APPDATA%\OpenOSV\defaults.json (roaming, so it follows the user between
// machines), or whatever OPENOSV_DEFAULTS_FILE names - the override exists for
// render farms and for the tests, which must never read or write the real one.
//
// Human readable JSON with ONE NAMED KEY PER SETTING, never a dump of the
// 128 bytes: the blob's layout grows (packages append fields to its reserved
// block), and a named key survives that where a byte offset would silently
// start meaning something else.  Reading is forgiving on purpose -
//
//   * a missing key keeps the built-in value (an older file simply predates
//     the setting);
//   * an unknown key is ignored (a newer file, or a hand edit);
//   * a key with a value this build does not understand keeps the built-in
//     value for that one setting;
//   * a file that is not JSON, or not ours, is logged once and ignored as a
//     whole: new clips get the built-in defaults, which is always safe.
//
// Writes are atomic (a temporary file in the same directory, flushed, then
// renamed over the old one), so a crash or a second Premiere writing at the
// same moment can never leave a half-written file for the next read.  Reads
// are cached per module and re-validated with the file's modification time,
// size and identity (every save is a new file), so asking for the defaults
// on every clip open costs one stat.
//
// ===========================================================================
//  WHO USES IT
// ===========================================================================
//
// OpenOSVImporter.prm (a new clip's first prefs), OpenOSVSourceSettings.aex
// (the Defaults group, and the seeding of a freshly applied effect) and
// osvtool (render --use-user-defaults).  None of them may depend on the
// others, so this file depends on nothing but PrefsBlob.h, osv_core and
// nlohmann-json: no Adobe header, no PluginLog.  Log lines go through a sink
// each module installs (setUserDefaultsLogSink); without one they reach the
// library's own osv::log facade.
#pragma once

#include "PrefsBlob.h"

#include "osv/core/Status.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace osv::premiere {

// ---------------------------------------------------------------------------
//  Constants
// ---------------------------------------------------------------------------

/// Environment variable that names the defaults file explicitly.  Set, it wins
/// over %APPDATA%; the tests point it at a private temporary file so a test
/// run can never read - or overwrite - the user's real defaults.
inline constexpr const char* kUserDefaultsFileEnvVar = "OPENOSV_DEFAULTS_FILE";

/// Wide spelling of kUserDefaultsFileEnvVar, for GetEnvironmentVariableW.
inline constexpr const wchar_t* kUserDefaultsFileEnvVarW = L"OPENOSV_DEFAULTS_FILE";

/// The "format" string every file carries, so a JSON file that merely
/// happens to sit at the same path is recognised as not ours.
inline constexpr const char* kUserDefaultsFormatName = "openosv-source-settings-defaults";

/// The file layout version this build writes.  Readers accept any version:
/// the keys are named, so a newer file's known keys still mean what they say
/// and its unknown ones are skipped.
inline constexpr int kUserDefaultsFormatVersion = 1;

/// Largest file that is read at all.  A real file is well under 1 KB; the
/// limit only stops a mistaken path (a video, a log) from being slurped into
/// memory on every clip open.
inline constexpr std::size_t kMaxUserDefaultsFileBytes = 64u * 1024u;

// ---------------------------------------------------------------------------
//  Results
// ---------------------------------------------------------------------------

/// The user defaults in force, and where they came from.
struct UserDefaults {
    /// The settings, sanitised: PrefsBlob::defaults() when no usable file
    /// exists, otherwise the file's values with the built-in ones filling
    /// every key the file does not carry.
    PrefsBlob prefs = PrefsBlob::defaults();
    /// The file that was consulted (empty when no location could be
    /// resolved - no %APPDATA% and no override).
    std::filesystem::path path;
    /// True when a readable, well-formed file supplied the values.  False
    /// means "the built-in defaults", whatever the reason (no file, a corrupt
    /// one, no location).
    bool fromFile = false;
};

/// A parsed defaults document.
struct UserDefaultsDocument {
    /// The settings, sanitised (built-in values for every key not given).
    PrefsBlob prefs = PrefsBlob::defaults();
    /// Things worth a log line that did not make the document unusable: an
    /// unknown key, a value this build does not understand, a newer layout
    /// version.  Plain ASCII sentences.
    std::vector<std::string> notes;
};

/// One named setting and the PrefsBlob bytes it is stored in, so a test can
/// prove the file covers every field of the blob (a field appended to the
/// blob without a key here would silently never be saved as a default).
struct UserDefaultsField {
    const char* key = nullptr;       ///< The JSON key inside "settings".
    std::size_t offset = 0;          ///< First byte of the field in PrefsBlob.
    std::size_t size = 0;            ///< Bytes of that field.
    std::size_t extraOffset = 0;     ///< A second field the key also writes (calibration's force-native byte).
    std::size_t extraSize = 0;       ///< 0 when there is no second field.
};

// ---------------------------------------------------------------------------
//  Logging
// ---------------------------------------------------------------------------

/// Severity of a message this module emits.
enum class UserDefaultsLogLevel : int { Debug = 0, Info = 1, Warn = 2, Error = 3 };

/// Where this module's log lines go.  Must not throw and must not call back
/// into this module.  Called outside every lock of this module.
using UserDefaultsLogSink = void (*)(UserDefaultsLogLevel level, std::string_view message) noexcept;

/// Install the sink for this module (each plug-in has its own copy of this
/// code, so each installs its own).  Null restores the default, which is the
/// library's osv::log facade.  Thread-safe.
void setUserDefaultsLogSink(UserDefaultsLogSink sink) noexcept;

// ---------------------------------------------------------------------------
//  The pure layer: text <-> settings, path-explicit file access
// ---------------------------------------------------------------------------

/// The field table, in the order the file lists the keys.
[[nodiscard]] std::span<const UserDefaultsField> userDefaultsFields() noexcept;

/// Serialise `prefs` (sanitised first) as the pretty-printed JSON document the
/// file holds, ending with a newline.  Empty only on allocation failure.
[[nodiscard]] std::string userDefaultsToJson(const PrefsBlob& prefs) noexcept;

/// Parse a document.  Fails (Malformed) only when the text is not JSON, its
/// top level is not an object, its "format" names something else or its
/// "settings" is not an object; every problem with a single key is a note and
/// that key keeps its built-in value.  Comments (// and /* */) are accepted,
/// so a user can annotate the file by hand.
[[nodiscard]] Result<UserDefaultsDocument> userDefaultsFromJson(std::string_view text) noexcept;

/// Read and parse `path`.  NotFound when there is no file, Io when it cannot
/// be read, Malformed when it is larger than kMaxUserDefaultsFileBytes or
/// userDefaultsFromJson() refuses it.
[[nodiscard]] Result<UserDefaultsDocument> readUserDefaultsFile(const std::filesystem::path& path) noexcept;

/// Write `prefs` to `path` atomically: the parent directory is created, the
/// document goes to a temporary file beside the target, is flushed to disk
/// and is then renamed over the target.  On failure the target is untouched
/// and the temporary file is removed.
[[nodiscard]] Status writeUserDefaultsFile(const std::filesystem::path& path, const PrefsBlob& prefs) noexcept;

/// Delete `path`.  A file that is already gone is success: the outcome the
/// caller asked for ("no user defaults") holds either way.
[[nodiscard]] Status removeUserDefaultsFile(const std::filesystem::path& path) noexcept;

/// The clip's stored blob when `bytes` holds one of ours (the result is
/// sanitised), otherwise the current user defaults.  `usedDefaults`, when not
/// null, receives the defaults that were consulted and `fromDefaults` whether
/// they were used - the caller logs "new clip" lines from those.  This is THE
/// rule of the whole feature: a valid stored blob always wins.
[[nodiscard]] PrefsBlob storedPrefsOrUserDefaults(const void* bytes, std::size_t length, UserDefaults* usedDefaults,
                                                  bool* fromDefaults) noexcept;

/// A one-line summary of `prefs` in the file's own tokens ("colour hlg, size
/// 1920x960, stab full, ..."), for log lines.  Empty on allocation failure.
[[nodiscard]] std::string userDefaultsSummary(const PrefsBlob& prefs) noexcept;

/// UTF-8 spelling of a path for a log line; never throws.
[[nodiscard]] std::string userDefaultsPathForLog(const std::filesystem::path& path) noexcept;

// ---------------------------------------------------------------------------
//  The process-wide layer: the user's own file
// ---------------------------------------------------------------------------

/// Where the user's defaults live: OPENOSV_DEFAULTS_FILE when set, otherwise
/// %APPDATA%\OpenOSV\defaults.json (on other systems $XDG_CONFIG_HOME, else
/// ~/.config, /openosv/defaults.json).  Empty when none of those resolve.
[[nodiscard]] std::filesystem::path userDefaultsPath() noexcept;

/// The user defaults in force, cached per module and re-read only when the
/// file's modification time, size, identity or path changed.  A corrupt file is logged
/// once per state and yields the built-in defaults.  Thread-safe.
[[nodiscard]] UserDefaults currentUserDefaults() noexcept;

/// Shorthand for currentUserDefaults().prefs.
[[nodiscard]] PrefsBlob userDefaults() noexcept;

/// Store `prefs` as the user's defaults (atomically, see
/// writeUserDefaultsFile) and refresh this module's cache.  Logs the outcome.
[[nodiscard]] Status saveUserDefaults(const PrefsBlob& prefs) noexcept;

/// Delete the user's defaults file so new clips start from the built-in
/// defaults again, and refresh this module's cache.  Logs the outcome.
[[nodiscard]] Status resetUserDefaults() noexcept;

}  // namespace osv::premiere
