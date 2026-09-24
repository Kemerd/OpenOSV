// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// PluginExport.h - how a plug-in module exports its entry points.
//
// Premiere and After Effects find EffectMain, xGPUFilterEntry and
// xImportEntry by name in the loaded module, so those symbols - and only
// those - must be visible from outside it:
//
//   * Windows: __declspec(dllexport) puts the name in the DLL's export table;
//   * macOS:   the bundles are compiled with -fvisibility=hidden (the same
//              "symbols private extern" setting Adobe's Xcode samples use,
//              so the statically linked library never leaks its thousands
//              of symbols into a host that loads several plug-ins), and
//              visibility("default") re-exposes the entry points.
//
// Clang on macOS rejects __declspec outright, which is why the spelling
// cannot simply be shared.  xImportEntry keeps the SDK's own DllExport
// (PrSDKEntry.h), which already resolves the same way.
#pragma once

#if defined(_WIN32)
#define OSV_PLUGIN_EXPORT __declspec(dllexport)
#else
#define OSV_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif
