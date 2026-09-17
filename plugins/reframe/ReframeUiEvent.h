// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ReframeUiEvent.h - the PF_Cmd_EVENT side of the Program Monitor overlay.
//
// This header names Adobe types, so it includes the Adobe headers itself and
// pulls in ReframeUi.h only AFTERWARDS.  That order is not cosmetic: the AE
// headers are wrapped in #pragma pack(push, 1), and ReframeUi.h declares
// structs of our own that must keep natural alignment.  Including it while
// Adobe's packing is in force would silently repack them and the interaction
// core compiled into the tests would then have a different layout from the
// one compiled into the plug-in.
//
// Everything the overlay does lives behind these three functions, so
// EffectMain.cpp gains three call sites and no knowledge of DrawBot.
#ifndef OSV_REFRAME_UI_EVENT_H
#define OSV_REFRAME_UI_EVENT_H

// The Adobe headers first, in the order the rest of this plug-in uses them.
#include "AEConfig.h"

#include "A.h"
#include "AE_Effect.h"
#include "AE_EffectCB.h"
#include "AE_EffectUI.h"

// Ours, after Adobe's packing has been popped.
#include "ReframeUi.h"

namespace osv::reframe::ui {

/// Fill in the PF_CustomUIInfo that PF_Cmd_PARAMS_SETUP hands back through
/// `out_data->custom_ui_info`, and register it with the host.
///
/// Two things happen here and both are required for the overlay to exist:
///
///   * `events` gets PF_CustomEFlag_COMP.  In After Effects that means "draw
///     in the composition window"; in Premiere the composition window IS the
///     Program Monitor, and the SDK's own note is that Premiere only routes
///     keyboard events to a custom UI that asked for COMP.  Without this
///     flag PF_Cmd_EVENT is never sent for the monitor at all.
///
///   * `comp_ui_width` / `comp_ui_height` stay ZERO.  They size a FIXED
///     overlay area; an overlay that has to track the picture as the user
///     scales the monitor must not pin itself to a fixed pixel box, and zero
///     is the documented "use the whole view" answer.
///
/// Returns PF_Err_NONE on success.  A null `out_data` is reported as
/// PF_Err_BAD_CALLBACK_PARAM and nothing is touched; the caller then simply
/// ships without an overlay rather than failing PARAMS_SETUP outright.
[[nodiscard]] PF_Err registerCustomUi(PF_InData* in_data, PF_OutData* out_data) noexcept;

/// Handle one PF_Cmd_EVENT.
///
/// Dispatches on `extra->e_type` and implements PF_Event_DRAW,
/// PF_Event_DO_CLICK, PF_Event_DRAG and PF_Event_ADJUST_CURSOR; every other
/// event type is accepted and ignored (returning an error for an event we do
/// not care about makes the host report a broken effect).
///
/// NEVER throws and never lets an exception cross back into the host: the
/// whole body is inside a try/catch and any failure degrades to "no overlay
/// this frame".  A null argument, a missing suite or a degenerate viewport
/// are all handled the same way - log once, leave PF_EO_HANDLED_EVENT clear,
/// return PF_Err_NONE.
///
/// `params` may be null for the context events; it is required (and checked)
/// for the ones that read or write values.
[[nodiscard]] PF_Err handleEvent(PF_InData* in_data, PF_OutData* out_data, PF_ParamDef* params[],
                                 PF_EventExtra* extra) noexcept;

/// Release anything the overlay cached for the process.
///
/// Called from PF_Cmd_GLOBAL_SETDOWN.  The only thing to release is the drag
/// table (see ReframeUiEvent.cpp), which is static storage rather than an
/// allocation, so this simply marks every slot free; it exists so a reloaded
/// plug-in cannot inherit a half-finished gesture from the previous load.
void shutdown() noexcept;

}  // namespace osv::reframe::ui

#endif /* OSV_REFRAME_UI_EVENT_H */
