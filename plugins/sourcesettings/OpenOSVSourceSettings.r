/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * OpenOSVSourceSettings.r - the PiPL resource of the Source Settings effect.
 *
 * Premiere reads this resource at scan time to decide whether to load the
 * module at all, what to call it, which bin to put it in and which entry
 * point to call.  It is turned into an .rc fragment by the three-step AE
 * pipeline that cmake/OsvPremiereSdk.cmake's osv_add_pipl() drives:
 *
 *     cl /EP  OpenOSVSourceSettings.r   -> .rr    (macros expanded)
 *     PiPLtool .rr                      -> .rrc   (binary resource)
 *     cl /EP  .rrc                       -> .rcp   (final .rc text)
 *
 * Premiere's own Cnvtpipl.exe CANNOT build an AE-kind PiPL - it rejects
 * CodeWin64X86 and AE_Effect_Global_OutFlags_2 outright - which is why the
 * After Effects SDK's PiPLtool.exe is used instead.
 *
 * THE MATCH NAME IS THE WHOLE INTERFACE.  Premiere pairs an importer with a
 * source settings effect purely by string comparison against
 * imFileInfoRec8::sourceSettingsMatchName, with no handshake and no
 * diagnostic on a mismatch.  It is therefore taken from
 * plugins/common/SourceSettingsIdentity.h, the same header the importer
 * includes, and a test reads this resource back out of the built module and
 * compares it to that header.
 *
 * This module carries exactly ONE PiPL.  The reframe effect is a separate
 * .aex because the AE SDK lists "multiple PiPLs in a single plug-in" among
 * the features Premiere does not support.
 */

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_General.r"

/* Our single source of truth.  cl /EP runs with /TC, so the C++ half of the
 * header is skipped and only the object-like macros below are visible.
 * SourceSettingsParams.h includes SourceSettingsIdentity.h for the names. */
#include "SourceSettingsParams.h"

resource 'PiPL' (16000) {
    {
        /* [1] AE-kind effect ('eFKT'), not a Premiere legacy filter. */
        Kind {
            AEEffect
        },

        /* [2] What the Effect Controls panel header shows. */
        Name {
            OSV_SOURCE_SETTINGS_DISPLAY_NAME
        },

        /* [3] The bin it appears under - the same one as the reframe effect,
         * so the two halves of the workflow sit together. */
        Category {
            OSV_SOURCE_SETTINGS_CATEGORY
        },

        /* [4] The exported entry point.  x64 Windows only: this repository
         * builds no Mac target. */
#ifdef AE_OS_WIN
        CodeWin64X86 {"EffectMain"},
#else
        CodeMacARM64 {"EffectMain"},
        CodeMacIntel64 {"EffectMain"},
#endif

        /* [5] PiPL structure version, always 2.0 for an AE effect. */
        AE_PiPL_Version {
            2,
            0
        },

        /* [6] Which AE effect API this was compiled against.  Taken from
         * AE_EffectVers.h so it always describes the SDK actually used. */
        AE_Effect_Spec_Version {
            PF_PLUG_IN_VERSION,
            PF_PLUG_IN_SUBVERS
        },

        /* [7] Our own version, packed exactly as PF_VERSION() packs it. */
        AE_Effect_Version {
            OSV_SOURCE_SETTINGS_PIPL_VERSION
        },

        /* [8] No info flags. */
        AE_Effect_Info_Flags {
            OSV_SOURCE_SETTINGS_INFO_FLAGS
        },

        /* [9] The out-flags, identical to what PF_Cmd_GLOBAL_SETUP reports.
         *     out_flags : SEND_UPDATE_PARAMS_UI
         *     out_flags2: PARAM_GROUP_START_COLLAPSED
         *                 | SUPPORTS_THREADED_RENDERING
         * Deliberately no colour-awareness flag of any kind: those describe
         * PF_Cmd_RENDER behaviour, and a source settings effect is never
         * sent PF_Cmd_RENDER. */
        AE_Effect_Global_OutFlags {
            OSV_SOURCE_SETTINGS_OUT_FLAGS
        },
        AE_Effect_Global_OutFlags_2 {
            OSV_SOURCE_SETTINGS_OUT_FLAGS_2
        },

        /* [10] The permanent identity.  The importer writes this exact string
         * into imFileInfoRec8::sourceSettingsMatchName. */
        AE_Effect_Match_Name {
            OSV_SOURCE_SETTINGS_MATCH_NAME
        },

        /* [11] Reserved; 8 in every AE-kind PiPL Adobe ships. */
        AE_Reserved_Info {
            OSV_SOURCE_SETTINGS_RESERVED_INFO
        }
    }
};
