// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DjmdDecoder: turns one raw djmd sample (a serialised ProductMeta message)
// into the typed structs of Types.h.
//
// Decoding is deliberately forgiving: every field is optional, unknown fields
// are skipped (and logged at debug level), a malformed sub-message is
// recorded in ProductMeta::warnings while the rest of the sample is still
// decoded.  Only a sample that yields no usable top-level field at all is
// reported as an error.
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Result.h"
#include "osv/meta/Types.h"

#include <string>
#include <vector>

namespace osv::meta {

class DjmdDecoder {
public:
    /// Decode a whole sample: ProductMeta {1 ClipMeta, 2 StreamMeta, 3 FrameMeta}.
    /// Returns InvalidArgument for an empty span and Malformed when the
    /// top-level scan produced nothing usable.
    [[nodiscard]] static Result<ProductMeta> decode(ByteSpan sample);

    /// Decode a ClipMeta message body (field 1 payload of ProductMeta).
    /// `warnings` (optional) receives non-fatal problems.
    [[nodiscard]] static Result<ClipMeta> decodeClipMeta(ByteSpan body, std::vector<std::string>* warnings = nullptr);

    /// Decode a StreamMeta message body (field 2 payload), including all 24
    /// PanoDewarpParams slots.
    [[nodiscard]] static Result<StreamMeta> decodeStreamMeta(ByteSpan body,
                                                             std::vector<std::string>* warnings = nullptr);

    /// Decode one DewarpParams record (a PanoDewarpParams slot payload).
    [[nodiscard]] static Result<DewarpParams> decodeDewarp(ByteSpan body, std::vector<std::string>* warnings = nullptr);

    /// Decode a FrameMeta message body (field 3 payload).
    [[nodiscard]] static Result<FrameMeta> decodeFrameMeta(ByteSpan body, std::vector<std::string>* warnings = nullptr);

    /// Decode a Quaternion message {1 w, 2 x, 3 y, 4 z}; sets `present`.
    [[nodiscard]] static Quaternion decodeQuaternion(ByteSpan body, std::vector<std::string>* warnings = nullptr);
};

}  // namespace osv::meta
