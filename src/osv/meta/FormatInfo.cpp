// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Out-of-line helpers for FormatInfo.h.  Kept apart from FormatDetector.cpp
// so code that only serialises a FormatInfo (MetaJson) does not pull in the
// container-dependent detector.

#include "osv/meta/FormatInfo.h"

namespace osv::meta {

const char* modeName(Mode mode) noexcept {
    // Every enumerator has a stable name; unknown numeric values (a newer
    // library writing into an older reader) collapse to "Unknown".
    switch (mode) {
    case Mode::Unknown: return "Unknown";
    case Mode::K4: return "K4";
    case Mode::K6: return "K6";
    case Mode::K8: return "K8";
    case Mode::Lrf: return "Lrf";
    }
    return "Unknown";
}

}  // namespace osv::meta
