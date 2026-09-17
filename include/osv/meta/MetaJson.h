// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MetaJson: JSON serialisation of every typed metadata struct, of FormatInfo
// and CalibrationSet, and of the schema-less ProtoTree.  Used by
// `osvtool probe --json` and by the golden tests (the key names match the
// documents written by scripts/gen_golden.py).
//
// Numbers are emitted with nlohmann::json's default formatting (shortest
// round-trip representation, up to 17 significant digits); every float is
// widened to double first so the exact stored value survives.
#pragma once

#include "osv/meta/FormatInfo.h"
#include "osv/meta/ProtoTree.h"
#include "osv/meta/Types.h"

#include <nlohmann/json.hpp>

namespace osv::meta {

/// Quaternion -> {"w","x","y","z"} or null when !present.
[[nodiscard]] nlohmann::json toJson(const Quaternion& q);
/// DewarpParams -> object with every field plus "presentFields".
[[nodiscard]] nlohmann::json toJson(const DewarpParams& d);
/// PanoDewarpParams -> object keyed by slot name; empty/absent slots are null.
[[nodiscard]] nlohmann::json toJson(const PanoDewarpParams& p);
/// ClipMeta -> object (header nested under "header").
[[nodiscard]] nlohmann::json toJson(const ClipMeta& c);
/// StreamMeta -> object ("video" and "dewarp" nested).
[[nodiscard]] nlohmann::json toJson(const StreamMeta& s);
/// ImuBatch -> object with the full quaternion list (null when !present).
[[nodiscard]] nlohmann::json toJson(const ImuBatch& b);
/// FrameMeta -> object ("camera" nested, "imu" null when absent).
[[nodiscard]] nlohmann::json toJson(const FrameMeta& f);
/// ProductMeta -> {"clip","stream","frame","warnings"} (absent parts null).
[[nodiscard]] nlohmann::json toJson(const ProductMeta& p);
/// FormatInfo -> object with enum numbers and their names.
[[nodiscard]] nlohmann::json toJson(const FormatInfo& f);
/// CalibrationSet -> {"sourceSlave","sourceMaster","slave","master"}.
[[nodiscard]] nlohmann::json toJson(const CalibrationSet& c);
/// ProtoNode -> {"field","wire",value...,"children"} for raw probe output.
[[nodiscard]] nlohmann::json toJson(const ProtoNode& n);
/// ProtoTree -> {"fields":[...],"failed","failure","nodeCount"}.
[[nodiscard]] nlohmann::json toJson(const ProtoTree& t);

}  // namespace osv::meta
