/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * Open360Reframe.r - the PiPL resource of the effect.
 *
 * Premiere reads this resource at scan time to decide whether to load the
 * module at all, what to call it, which bin to put it in and which entry
 * point to call.  It is turned into an .rc fragment by the three-step AE
 * pipeline that cmake/OsvPremiereSdk.cmake's osv_add_pipl() drives:
 *
 *     cl /EP  Open360Reframe.r   -> Open360Reframe.rr    (macros expanded)
 *     PiPLtool Open360Reframe.rr -> Open360Reframe.rrc   (binary resource)
 *     cl /EP  Open360Reframe.rrc -> Open360Reframe.rcp   (final .rc text)
 *
 * Premiere's own Cnvtpipl.exe CANNOT build an AE-kind PiPL - it rejects
 * CodeWin64X86 and AE_Effect_Global_OutFlags_2 outright - which is why the
 * After Effects SDK's PiPLtool.exe is used instead.
 *
 * Every value that also exists in the code comes from ReframeParams.h, and
 * EffectMain.cpp static_asserts the flag words against the real AE_Effect.h
 * constants.  A drift between this resource and PF_Cmd_GLOBAL_SETUP is a
 * documented way for an effect to be rejected at load time, so the two
 * cannot be allowed to be written twice.
 */

#include "AEConfig.h"
#include "AE_EffectVers.h"
#include "AE_General.r"

/* Our single source of truth.  cl /EP runs with /TC, so the C++ half of the
 * header is skipped and only the object-like macros below are visible. */
#include "ReframeParams.h"

resource 'PiPL' (16000) {
    {
        /* [1] AE-kind effect ('eFKT'), not a Premiere legacy filter. */
        Kind {
            AEEffect
        },

        /* [2] What the Effects panel shows. */
        Name {
            OSV_REFRAME_DISPLAY_NAME
        },

        /* [3] The bin it appears under. */
        Category {
            OSV_REFRAME_CATEGORY
        },

        /* [4] The exported entry point.  x64 Windows only: the CUDA path
         * has no ARM64 equivalent and this repository builds no Mac target. */
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
            OSV_REFRAME_PIPL_VERSION
        },

        /* [8] No info flags. */
        AE_Effect_Info_Flags {
            OSV_REFRAME_INFO_FLAGS
        },

        /* [9] The out-flags, identical to what PF_Cmd_GLOBAL_SETUP reports.
         *     out_flags : DEEP_COLOR_AWARE | SEND_UPDATE_PARAMS_UI | CUSTOM_UI
         *                 | USE_OUTPUT_EXTENT
         *     out_flags2: PARAM_GROUP_START_COLLAPSED | REVEALS_ZERO_ALPHA
         *                 | PRESERVES_FULLY_OPAQUE_PIXELS | FLOAT_COLOR_AWARE
         *                 | SUPPORTS_THREADED_RENDERING
         *     (ReframeParams.h explains each bit.) */
        AE_Effect_Global_OutFlags {
            OSV_REFRAME_OUT_FLAGS
        },
        AE_Effect_Global_OutFlags_2 {
            OSV_REFRAME_OUT_FLAGS_2
        },

        /* [10] The permanent identity.  xGPUFilterEntry reports the same
         * string, which is how the GPU renderer is bound to this effect. */
        AE_Effect_Match_Name {
            OSV_REFRAME_MATCH_NAME
        },

        /* [11] Reserved; 8 in every AE-kind PiPL Adobe ships. */
        AE_Reserved_Info {
            OSV_REFRAME_RESERVED_INFO
        }
    }
};
