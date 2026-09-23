// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// BundleProbe.cpp - the one source of osv_bundle_probe.bundle, a stand-in
// for a plug-in bundle built by the same osv_add_mac_bundle() the Premiere
// plug-ins use (tests/macos/CMakeLists.txt).  It calls into FFmpeg, so the
// post-build step has libraries to embed, and exports exactly the two
// functions test_bundle.cpp looks up.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include "PluginExport.h"

/// avutil_version() as the bundle's own copy of FFmpeg reports it.
extern "C" OSV_PLUGIN_EXPORT unsigned osvBundleProbeAvutilVersion() { return avutil_version(); }

/// The address of avcodec_version as this bundle resolved it: dladdr() on it
/// names the image that provides it, which must be the bundle's embedded,
/// renamed copy and nothing else.
extern "C" OSV_PLUGIN_EXPORT const void* osvBundleProbeAvcodecImage() {
    return reinterpret_cast<const void*>(&avcodec_version);
}

/// Not exported (hidden visibility, not in the export list): the test checks
/// dlsym cannot see it.
extern "C" unsigned osvBundleProbeHidden() { return avcodec_version(); }
