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

// Colour output radio group (a contiguous range: CheckRadioButton needs it).
#define IDC_COLOR_PQ  1001
#define IDC_COLOR_HLG 1002
#define IDC_COLOR_709 1003

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
