/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * SourceSettingsIdentity.h - the ONE place the Source Settings effect's
 * match name is written down.
 *
 * Premiere binds an importer to a source settings effect by STRING: the
 * importer fills imFileInfoRec8::sourceSettingsMatchName and the host then
 * looks for an installed effect whose PiPL AE_Effect_Match_Name is exactly
 * that text (PrSDKImport.h:425).  There is no handshake and no diagnostic: a
 * single mistyped character means the Effect Controls panel simply never
 * shows the stitch options, with nothing in any log to say why.
 *
 * So the string lives here, in plugins/common, and is included by all three
 * parties that must agree on it:
 *
 *   1. plugins/sourcesettings/OpenOSVSourceSettings.r  - the PiPL resource
 *      (through cl /EP, which is why the macro is object-like and the C++
 *      half is guarded);
 *   2. plugins/sourcesettings/SourceSettingsMain.cpp   - the effect, which
 *      static_asserts its own constant against the macro;
 *   3. plugins/importer/ImporterVideo.cpp              - the importer, which
 *      copies it, with the host's "AE." prefix, into sourceSettingsMatchName.
 *
 * A test additionally reads the built .aex's PiPL back out of the module and
 * compares it to this macro, so the resource and the code cannot drift even
 * if someone edits the .r by hand.
 *
 * Nothing here includes an Adobe header, and nothing here declares a struct,
 * so the file is safe to include on either side of an Adobe
 * #pragma pack(push, 1) region.
 */
#ifndef OSV_SOURCE_SETTINGS_IDENTITY_H
#define OSV_SOURCE_SETTINGS_IDENTITY_H

/* --------------------------------------------------------------------------
 *  Dialect detection.  cl /EP on the .r file runs with /TC (C, not C++), so
 *  __cplusplus is absent there and the C++ half below is skipped.  Same
 *  mechanism ReframeParams.h uses, for the same reason.
 * -------------------------------------------------------------------------- */
#if defined(__cplusplus)
#define OSV_SOURCE_SETTINGS_CPLUSPLUS 1
#endif

/* The permanent identity of the Source Settings effect.  Premiere stores it
 * in project files and the plug-in cache, and the importer names it in
 * sourceSettingsMatchName.  NEVER change this string: an existing project
 * would lose the master-clip effect that carries its stitch settings. */
#define OSV_SOURCE_SETTINGS_MATCH_NAME "OpenOSV.SourceSettings"

/* The name PREMIERE knows the effect by, which is what the importer must put
 * in imFileInfoRec8::sourceSettingsMatchName.
 *
 * Premiere registers every AE-API effect as "AE." + its PiPL match name (the
 * same reason the panel asks for "AE.OpenOSV.Open360Reframe").  Given the
 * bare PiPL name, Premiere still finds and instantiates the effect, but it
 * never recognises the instance already on the master clip as that effect.
 * So every time it re-checks the clip (the Master tab, a sequence settings
 * change, a proxy attach) it adds ANOTHER Source Settings effect, and a source
 * settings effect cannot be deleted.  A third-party importer developer on
 * Adobe's forum hit exactly this and fixed it with the prefix.
 *
 * Derived from the permanent name above, never spelled out separately. */
#define OSV_SOURCE_SETTINGS_HOST_MATCH_NAME "AE." OSV_SOURCE_SETTINGS_MATCH_NAME

/* Displayed in the Effect Controls panel header and in the Effects panel. */
#define OSV_SOURCE_SETTINGS_DISPLAY_NAME "OpenOSV Source Settings"

/* Bin in the Effects panel.  The same bin as the reframe effect, so the two
 * halves of the workflow sit together. */
#define OSV_SOURCE_SETTINGS_CATEGORY "OpenOSV"

#if defined(OSV_SOURCE_SETTINGS_CPLUSPLUS)

namespace osv::premiere {

/// The match name as a C++ constant, for the importer's
/// `sourceSettingsMatchName` copy and for the tests.  Identical to
/// OSV_SOURCE_SETTINGS_MATCH_NAME by construction - it is defined FROM the
/// macro rather than repeated, which is the only way two spellings cannot
/// diverge.
inline constexpr const char* kSourceSettingsMatchName = OSV_SOURCE_SETTINGS_MATCH_NAME;

/// The same string as UTF-16.  Spelled with the L prefix applied to the
/// macro so the two literals are generated from one token sequence.
inline constexpr const wchar_t* kSourceSettingsMatchNameW = L"" OSV_SOURCE_SETTINGS_MATCH_NAME;

/// The host's name for the effect ("AE." + the match name) as UTF-16,
/// because imFileInfoRec8::sourceSettingsMatchName is a prUTF16Char[256]
/// field.  This, not the bare match name, is what the importer advertises
/// (see OSV_SOURCE_SETTINGS_HOST_MATCH_NAME for the duplicate effects the
/// bare name caused).
inline constexpr const wchar_t* kSourceSettingsHostMatchNameW = L"" OSV_SOURCE_SETTINGS_HOST_MATCH_NAME;

}  // namespace osv::premiere

#endif /* OSV_SOURCE_SETTINGS_CPLUSPLUS */

#endif /* OSV_SOURCE_SETTINGS_IDENTITY_H */
