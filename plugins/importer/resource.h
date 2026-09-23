// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Resource identifiers shared by OpenOSVImporter.rc and
// SourceSettingsDialog.cpp.  Plain #defines because a .rc file is compiled by
// rc.exe, which understands the preprocessor but not C++.
//
// The IDs never change: a dialog template and its control IDs are part of the
// binary's ABI with its own resources, and renumbering them silently breaks
// every GetDlgItem call.
#pragma once

// The IMPT resource Premiere looks for to recognise an importer.  The SDK
// documents id 1000 (doc 4.3 / 7.3.17).
#define IDR_IMPT 1000

// The Source Settings dialog template.
#define IDD_SOURCE_SETTINGS 100

// Colour output.
//
// This was a three-way BS_AUTORADIOBUTTON group (IDC_COLOR_PQ / _HLG / _709).
// It is a combo box now, for two reasons:
//
//   * a fourth option (D-Log M passthrough) had to be added, and a radio
//     group grows by one CONTROL plus one hand-placed x coordinate every
//     time - the three buttons were laid out at x = 16 / 92 / 168 inside a
//     254-unit group box, so a fourth would not have fitted without
//     re-flowing the whole dialog;
//   * every other multi-choice setting in this dialog is already a combo box,
//     so the odd one out was the radio group.
//
// The old IDs are deliberately NOT reused for anything else: a stale
// GetDlgItem(IDC_COLOR_PQ) would then silently find a real but wrong control
// instead of returning null.  They are retired, and the reason is recorded
// here so nobody "tidies up" by reclaiming the numbers.
#define IDC_COLOR_OUTPUT   1004
// Retired: 1001, 1002, 1003 (the former PQ / HLG / 709 radio buttons).

// Everything else.
#define IDC_OUTPUT_SIZE    1010
#define IDC_STABILIZATION  1011
#define IDC_SEAM_SEARCH    1012
#define IDC_GAIN_MATCH     1013
#define IDC_CALIBRATION    1014
#define IDC_DLOGM_FIT      1015
#define IDC_EXPOSURE       1016
#define IDC_RENDER_DEVICE  1017

// Static labels (no code touches them; they exist so the .rc is readable).
#define IDC_STATIC_COLOR   1100
#define IDC_STATIC_SIZE    1101
#define IDC_STATIC_STAB    1102
#define IDC_STATIC_CALIB   1103
#define IDC_STATIC_FIT     1104
#define IDC_STATIC_EXP     1105
#define IDC_STATIC_DEVICE  1106
#define IDC_STATIC_STOPS   1107

// [WP-LOOK] The Rec.709 output's display look (DJI Studio / OpenOSV standard).
// Numbered well clear of the sequential block above so a control another
// change appends there cannot collide with it.
#define IDC_REC709_LOOK    1030
#define IDC_STATIC_LOOK    1130
