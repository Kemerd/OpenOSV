// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// A minimal DLL that sits next to the test executable so the DelayLoad test
// can prove the hook resolves a module from the plug-in's own directory.
extern "C" __declspec(dllexport) int osvDelayLoadProbe(void) { return 424242; }
