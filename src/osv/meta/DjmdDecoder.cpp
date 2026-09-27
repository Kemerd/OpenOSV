// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Typed decoding of the dvtm ProductMeta message carried by djmd samples.
// The field numbers used here are the ones documented in Types.h and in
// proto/dvtm_osmo360.proto; keep the three in sync.

#include "osv/meta/DjmdDecoder.h"

#include "osv/core/Log.h"
#include "osv/meta/ProtoWire.h"

#include <cmath>
#include <format>
#include <optional>

namespace osv::meta {

namespace {

// -----------------------------------------------------------------------------
//  Small utilities
// -----------------------------------------------------------------------------

/// Record a non-fatal problem: appended to the caller's list (when given)
/// and echoed at debug level so `-v` shows it.
void addWarning(std::vector<std::string>* warnings, std::string text) {
    log::debug("djmd: {}", text);
    if (warnings) {
        warnings->push_back(std::move(text));
    }
}

/// Note an unknown field (dropped).  Debug level only; a firmware newer than
/// our schema will produce these on every frame.
void noteUnknown(const char* context, const ProtoField& field) {
    if (log::enabled(log::Level::Debug)) {
        log::debug("djmd: {}: unknown field {} ({}) dropped", context ? context : "?", field.number,
                   wireTypeName(field.wire));
    }
}

/// Drive a scanner over `body`, invoking `onField` for every field, and turn
/// a scan failure into a warning that names `context`.  Returns true when
/// the whole body scanned cleanly.
template <class OnField>
bool scanMessage(ByteSpan body, const char* context, std::vector<std::string>* warnings, OnField&& onField) {
    ProtoScanner scanner(body);
    ProtoField field;
    while (scanner.next(field)) {
        onField(field);
    }
    if (scanner.failed()) {
        addWarning(warnings, std::format("{}: malformed at byte {} of {}: {}", context ? context : "?",
                                         scanner.pos(), scanner.size(), scanner.failure()));
        return false;
    }
    return true;
}

/// True when `field` is an embedded message (length-delimited).  Anything
/// else where a message is expected is reported and ignored.
bool expectMessage(const ProtoField& field, const char* context, std::vector<std::string>* warnings) {
    if (field.wire == WireType::LengthDelimited) {
        return true;
    }
    addWarning(warnings, std::format("{}: field {} expected a message but has wire type {}",
                                     context ? context : "?", field.number, wireTypeName(field.wire)));
    return false;
}

/// Pixel dimension stored either as a fixed32 float (what the Osmo 360
/// writes for DewarpParams.width/height: 3840.0f) or as a plain varint.
/// Negative, non-finite or absurdly large values collapse to 0 so a corrupt
/// record can never pass DewarpParams::hasCore().
std::uint32_t dimensionU32(const ProtoField& field) noexcept {
    if (field.wire == WireType::Fixed32 || field.wire == WireType::Fixed64) {
        const double v = field.asDouble();
        if (!std::isfinite(v) || v < 0.0 || v > 65535.0) {
            return 0;
        }
        return static_cast<std::uint32_t>(std::lround(v));
    }
    return field.asU32();
}

/// Report a repeated-scalar helper failure (wrong wire type / partial value).
void warnRepeated(bool ok, const char* context, const ProtoField& field, std::vector<std::string>* warnings) {
    if (!ok) {
        addWarning(warnings, std::format("{}: field {} is not a valid repeated scalar ({})",
                                         context ? context : "?", field.number, wireTypeName(field.wire)));
    }
}

// -----------------------------------------------------------------------------
//  Wrapper messages.  DJI wraps nearly every scalar in a one-field message
//  (`{1 value}`); these helpers unwrap them.  A missing field 1 leaves the
//  optional empty so the caller can keep its default.
// -----------------------------------------------------------------------------

/// Field 1 of a wrapper message, if present.
std::optional<ProtoField> wrappedField(ByteSpan body, const char* context, std::vector<std::string>* warnings) {
    std::optional<ProtoField> result;
    scanMessage(body, context, warnings, [&](const ProtoField& f) {
        if (f.number == 1) {
            result = f;  // proto3: last occurrence wins
        } else {
            noteUnknown(context, f);
        }
    });
    return result;
}

float wrappedFloat(ByteSpan body, const char* ctx, std::vector<std::string>* w, float fallback = 0.0f) {
    const auto f = wrappedField(body, ctx, w);
    return f ? f->asFloat() : fallback;
}

std::uint32_t wrappedU32(ByteSpan body, const char* ctx, std::vector<std::string>* w, std::uint32_t fallback = 0) {
    const auto f = wrappedField(body, ctx, w);
    return f ? f->asU32() : fallback;
}

std::int32_t wrappedI32(ByteSpan body, const char* ctx, std::vector<std::string>* w, std::int32_t fallback = 0) {
    const auto f = wrappedField(body, ctx, w);
    return f ? f->asI32() : fallback;
}

std::uint64_t wrappedU64(ByteSpan body, const char* ctx, std::vector<std::string>* w, std::uint64_t fallback = 0) {
    const auto f = wrappedField(body, ctx, w);
    return f ? f->asU64() : fallback;
}

/// Wrapper around a repeated float (`{1 repeated float}`), packed or not.
std::vector<float> wrappedRepeatedFloat(ByteSpan body, const char* ctx, std::vector<std::string>* w) {
    std::vector<float> out;
    scanMessage(body, ctx, w, [&](const ProtoField& f) {
        if (f.number == 1) {
            warnRepeated(appendRepeatedFloat(f, out), ctx, f, w);
        } else {
            noteUnknown(ctx, f);
        }
    });
    return out;
}

/// Wrapper around a repeated int32.
std::vector<std::int32_t> wrappedRepeatedI32(ByteSpan body, const char* ctx, std::vector<std::string>* w) {
    std::vector<std::int32_t> out;
    scanMessage(body, ctx, w, [&](const ProtoField& f) {
        if (f.number == 1) {
            warnRepeated(appendRepeatedI32(f, out), ctx, f, w);
        } else {
            noteUnknown(ctx, f);
        }
    });
    return out;
}

/// Wrapper around a repeated uint32.
std::vector<std::uint32_t> wrappedRepeatedU32(ByteSpan body, const char* ctx, std::vector<std::string>* w) {
    std::vector<std::uint32_t> out;
    scanMessage(body, ctx, w, [&](const ProtoField& f) {
        if (f.number == 1) {
            warnRepeated(appendRepeatedU32(f, out), ctx, f, w);
        } else {
            noteUnknown(ctx, f);
        }
    });
    return out;
}

// -----------------------------------------------------------------------------
//  ClipMeta sub-messages
// -----------------------------------------------------------------------------

/// ClipMetaHeader {1 proto_file_name, 2 library_proto_version,
/// 3 product_proto_version, 5 product_sn, 6 product_firmware_version,
/// 7 enum, 8 enum, 9 clip_timestamp, 10 product_name}.
void decodeClipHeader(ByteSpan body, ClipHeader& out, std::vector<std::string>* w) {
    scanMessage(body, "ClipMeta.header", w, [&](const ProtoField& f) {
        switch (f.number) {
        case 1: out.protoFileName = f.asString(); break;
        case 2: out.libVersion = f.asString(); break;
        case 3: out.productProtoVersion = f.asString(); break;
        case 5: out.serialNumber = f.asString(); break;
        case 6: out.firmware = f.asString(); break;
        case 7:
        case 8:
            // Enumerations whose meaning is not catalogued; nothing to keep.
            break;
        case 9: out.clipTimestampUs = f.asU64(); break;
        case 10: out.productName = f.asString(); break;
        default: noteUnknown("ClipMeta.header", f); break;
        }
    });
}

// -----------------------------------------------------------------------------
//  Schemas: each camera's field numbers mapped onto the Osmo 360's
// -----------------------------------------------------------------------------
//
//  Every DJI camera builds its ProductMeta from the same library messages,
//  but numbers the fields of four of them - ClipMeta, StreamMeta, FrameMeta
//  and FrameMetaOfCamera - its own way.  Everything below those four
//  (DewarpParams, PanoDewarpParams, Quaternion, the scalar wrappers, the IMU
//  batches) is shared and numbered identically on every camera.
//
//  Rather than a second copy of every decoder, each schema maps its own field
//  numbers onto the canonical ones - the Osmo 360's, which Types.h documents -
//  before the single set of switch statements below sees them.  `present`
//  bits therefore always carry canonical numbers, so FormatDetector's
//  "colour mode = StreamMeta bit 4" means the same on every camera.
//
//  The Avata 360's numbering is its schema as used by DJI Studio for Windows.
//  Its StreamMeta shape - camera_stream_meta at 2 with the colour mode at
//  2.4, fov_type at 4 (empty on every clip seen), pano_dewarp_params at 5 -
//  was checked against three recorded Avata 360 clips by a contributor.
// -----------------------------------------------------------------------------

/// One message's product field number -> canonical field number table.
/// Index = the product's field number; 0 = a field the canonical message
/// does not have (dropped and noted at debug level).  A null table is the
/// identity: the Osmo 360 itself, and every message that is not remapped.
struct FieldMap {
    const std::uint8_t* to = nullptr;  ///< Canonical number per product number.
    std::size_t size = 0;              ///< Entries in `to`.
};

/// The four remapped messages of one schema.
struct SchemaMaps {
    FieldMap clipMeta;     ///< ClipMeta (ProductMeta 1).
    FieldMap streamMeta;   ///< StreamMeta (ProductMeta 2).
    FieldMap frameMeta;    ///< FrameMeta (ProductMeta 3).
    FieldMap cameraFrame;  ///< FrameMetaOfCamera (FrameMeta 2).
};

// ---- Osmo 360 ----------------------------------------------------------------

/// The Osmo 360's StreamMeta is the canonical one except that it has no
/// field 2: camera_stream_meta is canonical only so the Avata 360's colour
/// mode has a home, and an Osmo 360 field 2 must stay an unknown field.
constexpr std::uint8_t kOsmoStreamMeta[] = {
    0,  // 0: never a field number
    1,  // 1 stream_meta_header
    0,  // 2 (not in the Osmo 360 schema)
    3,  // 3 video_stream_meta
    4,  // 4 color_mode
    5,  // 5 fov_type
    6,  // 6 pano_dewarp_params
    7,  // 7 extri_lens_mode
    8,  // 8 shading_calib_mode_num
};

// ---- Avata 360 ---------------------------------------------------------------

constexpr std::uint8_t kAvataClipMeta[] = {
    0,   // 0: never a field number
    1,   // 1 clip_meta_header
    2,   // 2 clip_streams_meta
    3,   // 3 distortion_coefficients
    4,   // 4 sensor_readout_time
    5,   // 5 sensor_read_direction
    8,   // 6 digital_focal_length
    9,   // 7 eis_status
    10,  // 8 imu_sampling_rate
    11,  // 9 sensor_fps
    12,  // 10 ITD_value
    13,  // 11 LRO_value
    14,  // 12 sensor_res
    16,  // 13 style_filter_mode
    0,   // 14 flat_res (no Osmo 360 counterpart)
    17,  // 15 yltm_enable
};

constexpr std::uint8_t kAvataStreamMeta[] = {
    0,  // 0: never a field number
    1,  // 1 stream_meta_header
    2,  // 2 camera_stream_meta (its field 4 is the colour mode)
    3,  // 3 video_stream_meta
    5,  // 4 fov_type
    6,  // 5 pano_dewarp_params
    7,  // 6 extri_lens_mode
};

constexpr std::uint8_t kAvataFrameMeta[] = {
    0,  // 0: never a field number
    1,  // 1 frame_meta_header
    2,  // 2 camera_frame_meta
    3,  // 3 imu_frame_meta
    0,  // 4 drone_frame_meta (the aircraft's own telemetry; not used)
    4,  // 5 gimbal_frame_meta
    5,  // 6 ebike_frame_meta
};

constexpr std::uint8_t kAvataCameraFrame[] = {
    0,   // 0: never a field number
    1,   // 1 camera_dev_header
    2,   // 2 exposure_index
    3,   // 3 iso
    4,   // 4 exposure_time
    5,   // 5 digital_zoom_ratio
    6,   // 6 white_balance_cct
    7,   // 7 orientation
    0,   // 8 exposure_value
    0,   // 9 color_temp_atmosphere
    0,   // 10 focal_length
    0,   // 11 absolute_altitude
    16,  // 12 sensor_temperature
    0,   // 13 sensor_active_size
    0,   // 14 f_number
    0,   // 15 digital_focal_length (per frame; the clip-level one is used)
    0,   // 16 sensor_shift_param
    0,   // 17 fancy_mode
    0,   // 18 liveview_hfov
    0,   // 19 aec_eagle
    0,   // 20 expo_balance_mode
    8,   // 21 current_underwater_confidence
    9,   // 22 camera_attitude
    10,  // 23 camera_acc
    11,  // 24 sharpness
    12,  // 25 denoising
    14,  // 26 track_ret
    15,  // 27 aec_ae
    0,   // 28 track_result_box_3d
    0,   // 29 track_roi_box_list_sur
};

/// Wrap a table as a FieldMap (the size comes from the array itself).
template <std::size_t N>
constexpr FieldMap fieldMap(const std::uint8_t (&table)[N]) noexcept {
    return FieldMap{table, N};
}

/// The tables of `schema`.
SchemaMaps schemaMaps(DjmdSchema schema) noexcept {
    switch (schema) {
    case DjmdSchema::Avata360:
        return SchemaMaps{fieldMap(kAvataClipMeta), fieldMap(kAvataStreamMeta), fieldMap(kAvataFrameMeta),
                          fieldMap(kAvataCameraFrame)};
    case DjmdSchema::Osmo360: break;
    }
    // The Osmo 360, and anything out of range: canonical numbering throughout.
    return SchemaMaps{FieldMap{}, fieldMap(kOsmoStreamMeta), FieldMap{}, FieldMap{}};
}

/// `raw` renumbered into the canonical numbering.  Number 0 means the
/// canonical message has no such field (a number past the table included).
ProtoField canonicalField(const ProtoField& raw, const FieldMap& map) noexcept {
    ProtoField f = raw;
    if (map.to != nullptr) {
        f.number = raw.number < map.size ? map.to[raw.number] : 0u;
    }
    return f;
}

/// The proto_file_name in a ClipMeta body's header (field 1.1), or empty.
/// A cheap pre-scan: ClipMeta's own numbering depends on it.
std::string clipProtoFileName(ByteSpan clipBody) {
    std::string name;
    // Silent scans: decodeClipMeta scans the same bytes again and reports
    // anything malformed there, once.
    scanMessage(clipBody, "ClipMeta", nullptr, [&](const ProtoField& f) {
        if (f.number != 1 || f.wire != WireType::LengthDelimited) {
            return;
        }
        scanMessage(f.bytes, "ClipMeta.header", nullptr, [&](const ProtoField& g) {
            if (g.number == 1 && g.wire == WireType::LengthDelimited) {
                name = g.asString();  // proto3: last occurrence wins
            }
        });
    });
    return name;
}

}  // namespace

// -----------------------------------------------------------------------------
//  Quaternion
// -----------------------------------------------------------------------------

Quaternion DjmdDecoder::decodeQuaternion(ByteSpan body, std::vector<std::string>* warnings) {
    Quaternion q;
    // Zero everything first: a present quaternion with missing components is
    // reported as zeros (the camera writes all-zero quaternions for unused
    // slots), not as the identity default of the struct.
    q.w = 0.0f;
    q.x = 0.0f;
    q.y = 0.0f;
    q.z = 0.0f;
    q.present = true;
    scanMessage(body, "Quaternion", warnings, [&](const ProtoField& f) {
        switch (f.number) {
        case 1: q.w = f.asFloat(); break;
        case 2: q.x = f.asFloat(); break;
        case 3: q.y = f.asFloat(); break;
        case 4: q.z = f.asFloat(); break;
        default: noteUnknown("Quaternion", f); break;
        }
    });
    return q;
}

// -----------------------------------------------------------------------------
//  ClipMeta
// -----------------------------------------------------------------------------

Result<ClipMeta> DjmdDecoder::decodeClipMeta(ByteSpan body, std::vector<std::string>* warnings) {
    ClipMeta clip;
    std::size_t decoded = 0;
    // The header (field 1 on every camera) names the schema, and the schema
    // numbers every other field of this very message: read the name first.
    const FieldMap map = schemaMaps(djmdSchemaForProto(clipProtoFileName(body))).clipMeta;
    const bool clean = scanMessage(body, "ClipMeta", warnings, [&](const ProtoField& raw) {
        const ProtoField f = canonicalField(raw, map);
        // Every known field lives in an embedded message; anything else with
        // a known number is malformed and reported (once, below the switch).
        const bool isMsg = f.wire == WireType::LengthDelimited;
        switch (f.number) {
        case 1:
            if (isMsg) {
                decodeClipHeader(f.bytes, clip.header, warnings);
            }
            break;
        case 2:
            if (isMsg) {
                scanMessage(f.bytes, "ClipMeta.clip_streams_meta", warnings, [&](const ProtoField& g) {
                    switch (g.number) {
                    case 1: clip.videoStreamCount = g.asU32(); break;
                    case 2: clip.audioStreamCount = g.asU32(); break;
                    default: noteUnknown("ClipMeta.clip_streams_meta", g); break;
                    }
                });
            }
            break;
        case 3:
            if (isMsg) {
                clip.distortionCoefficients = wrappedRepeatedFloat(f.bytes, "ClipMeta.distortion_coefficients", warnings);
            }
            break;
        case 4:
            if (isMsg) {
                clip.sensorReadoutTime = wrappedU64(f.bytes, "ClipMeta.sensor_readout_time", warnings);
            }
            break;
        case 5:
            if (isMsg) {
                clip.sensorReadDirection = wrappedI32(f.bytes, "ClipMeta.sensor_read_direction", warnings);
            }
            break;
        case 8:
            if (isMsg) {
                clip.digitalFocalLength = wrappedFloat(f.bytes, "ClipMeta.digital_focal_length", warnings);
            }
            break;
        case 9:
            if (isMsg) {
                clip.eisStatus = static_cast<EisStatus>(wrappedI32(f.bytes, "ClipMeta.eis_status", warnings));
            }
            break;
        case 10:
            if (isMsg) {
                clip.imuSamplingRate = wrappedU32(f.bytes, "ClipMeta.imu_sampling_rate", warnings);
            }
            break;
        case 11:
            if (isMsg) {
                clip.sensorFps = wrappedFloat(f.bytes, "ClipMeta.sensor_fps", warnings);
            }
            break;
        case 12:
            if (isMsg) {
                clip.itd = wrappedI32(f.bytes, "ClipMeta.ITD_value", warnings);
            }
            break;
        case 13:
            if (isMsg) {
                clip.lro = wrappedU32(f.bytes, "ClipMeta.LRO_value", warnings);
            }
            break;
        case 14:
            if (isMsg) {
                scanMessage(f.bytes, "ClipMeta.sensor_res", warnings, [&](const ProtoField& g) {
                    switch (g.number) {
                    case 1: clip.sensorW = g.asU32(); break;
                    case 2: clip.sensorH = g.asU32(); break;
                    default: noteUnknown("ClipMeta.sensor_res", g); break;
                    }
                });
            }
            break;
        case 15:
            if (isMsg) {
                clip.fNumber = wrappedRepeatedU32(f.bytes, "ClipMeta.f_number", warnings);
            }
            break;
        case 16:
            if (isMsg) {
                clip.styleFilterMode = wrappedU32(f.bytes, "ClipMeta.style_filter_mode", warnings);
            }
            break;
        case 17:
            if (isMsg) {
                clip.yltmEnable = wrappedU32(f.bytes, "ClipMeta.yltm_enable", warnings);
            }
            break;
        default:
            // Reported under the number the camera wrote, not the canonical one.
            noteUnknown("ClipMeta", raw);
            return;
        }
        // Wrong wire type for a known wrapped field: say so once.
        if (!isMsg) {
            expectMessage(raw, "ClipMeta", warnings);
            return;
        }
        if (f.number < clip.present.size()) {
            clip.present.set(f.number);
        }
        ++decoded;
    });
    if (decoded == 0 && !clean) {
        return Error{ErrorCode::Malformed, "ClipMeta: no decodable field"};
    }
    return clip;
}

// -----------------------------------------------------------------------------
//  DewarpParams
// -----------------------------------------------------------------------------

Result<DewarpParams> DjmdDecoder::decodeDewarp(ByteSpan body, std::vector<std::string>* warnings) {
    DewarpParams d;
    std::size_t decoded = 0;
    const char* ctx = "DewarpParams";
    const bool clean = scanMessage(body, ctx, warnings, [&](const ProtoField& f) {
        switch (f.number) {
        case 1: d.fx = f.asFloat(); break;
        case 2: d.fy = f.asFloat(); break;
        case 3: d.cx = f.asFloat(); break;
        case 4: d.cy = f.asFloat(); break;
        case 5: d.k[0] = f.asFloat(); break;
        case 6: d.k[1] = f.asFloat(); break;
        case 7: d.k[2] = f.asFloat(); break;
        case 8: d.k[3] = f.asFloat(); break;
        case 9: d.xi = f.asFloat(); break;
        // width/height are written as fixed32 floats by the camera (3840.0)
        // even though they are pixel counts; accept a varint as well.
        case 10: d.width = dimensionU32(f); break;
        case 11: d.height = dimensionU32(f); break;
        case 12: d.yaw = f.asFloat(); break;
        case 13: d.pitch = f.asFloat(); break;
        case 14: d.roll = f.asFloat(); break;
        case 15: d.k[4] = f.asFloat(); break;
        case 16: d.k[5] = f.asFloat(); break;
        case 17: d.k[6] = f.asFloat(); break;
        case 18: d.k[7] = f.asFloat(); break;
        case 19: d.k[8] = f.asFloat(); break;
        case 20: warnRepeated(appendRepeatedFloat(f, d.p), ctx, f, warnings); break;
        case 21: warnRepeated(appendRepeatedFloat(f, d.q), ctx, f, warnings); break;
        case 22: warnRepeated(appendRepeatedFloat(f, d.occlusionPtX), ctx, f, warnings); break;
        case 23: warnRepeated(appendRepeatedFloat(f, d.occlusionPtY), ctx, f, warnings); break;
        case 24: d.lensModel = f.asFloat(); break;
        case 25: d.temperature = f.asFloat(); break;
        case 26:
            if (expectMessage(f, ctx, warnings)) {
                d.camImuExtriQ = decodeQuaternion(f.bytes, warnings);
            }
            break;
        case 27: warnRepeated(appendRepeatedFloat(f, d.tangentCoeff), ctx, f, warnings); break;
        case 28:
            if (expectMessage(f, ctx, warnings)) {
                d.camExtriQ = decodeQuaternion(f.bytes, warnings);
            }
            break;
        case 29: d.camImuCaliEnable = f.asBool(); break;
        case 30: d.tempCompenEnable = f.asBool(); break;
        case 31: d.tempCompenK = f.asFloat(); break;
        case 32: d.gimbalYawH1 = f.asFloat(); break;
        case 33: d.gimbalYawH2 = f.asFloat(); break;
        case 34:
            // gimbal_self_cail_param: structure unknown, content unused.
            break;
        case 35: warnRepeated(appendRepeatedFloat(f, d.tempCompenKOrder), ctx, f, warnings); break;
        default:
            noteUnknown(ctx, f);
            return;
        }
        if (f.number < d.present.size()) {
            d.present.set(f.number);
        }
        ++decoded;
    });
    if (decoded == 0 && !clean) {
        return Error{ErrorCode::Malformed, "DewarpParams: no decodable field"};
    }
    return d;
}

// -----------------------------------------------------------------------------
//  StreamMeta
// -----------------------------------------------------------------------------

Result<StreamMeta> DjmdDecoder::decodeStreamMeta(ByteSpan body, std::vector<std::string>* warnings,
                                                 DjmdSchema schema) {
    StreamMeta s;
    std::size_t decoded = 0;
    const FieldMap map = schemaMaps(schema).streamMeta;
    const bool clean = scanMessage(body, "StreamMeta", warnings, [&](const ProtoField& raw) {
        if (!expectMessage(raw, "StreamMeta", warnings)) {
            return;
        }
        const ProtoField f = canonicalField(raw, map);
        switch (f.number) {
        case 1:
            scanMessage(f.bytes, "StreamMeta.header", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 1: s.id = g.asU32(); break;
                case 2: s.type = g.asI32(); break;
                case 3: s.name = g.asString(); break;
                default: noteUnknown("StreamMeta.header", g); break;
                }
            });
            break;
        case 2:
            // camera_stream_meta {1 device_header, 2 device_version,
            // 3 device_sn, 4 color_mode, 5 sharpness, 6 denoise}: the Avata
            // 360 keeps its colour mode here, in the same library ColorMode
            // wrapper the Osmo 360 writes at StreamMeta 4, so the value means
            // the same (19 D-Log M, 9 HLG, an empty wrapper Normal).  The
            // rest is identification the stitcher does not need.
            scanMessage(f.bytes, "StreamMeta.camera_stream_meta", warnings, [&](const ProtoField& g) {
                if (g.number != 4) {
                    return;
                }
                if (!expectMessage(g, "StreamMeta.camera_stream_meta", warnings)) {
                    return;
                }
                s.colorMode = static_cast<ColorMode>(
                    wrappedI32(g.bytes, "StreamMeta.camera_stream_meta.color_mode", warnings));
                // Canonical bit 4 is "the colour mode was recorded".
                s.present.set(4);
            });
            break;
        case 3:
            scanMessage(f.bytes, "StreamMeta.video_stream_meta", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 1: s.video.width = g.asU32(); break;
                case 2: s.video.height = g.asU32(); break;
                case 3: s.video.fps = g.asFloat(); break;
                case 4: s.video.bitDepthValid = g.asBool(); break;
                case 5: s.video.bitDepth = g.asU32(); break;
                case 6: s.video.bitFormat = g.asI32(); break;
                case 7: s.video.streamType = g.asI32(); break;
                case 8: s.video.codec = g.asI32(); break;
                default: noteUnknown("StreamMeta.video_stream_meta", g); break;
                }
            });
            break;
        case 4:
            s.colorMode = static_cast<ColorMode>(wrappedI32(f.bytes, "StreamMeta.color_mode", warnings));
            break;
        case 5:
            s.fovType = wrappedI32(f.bytes, "StreamMeta.fov_type", warnings);
            break;
        case 6:
            // PanoDewarpParams: fields 1..24 are DewarpParams records.
            scanMessage(f.bytes, "StreamMeta.pano_dewarp_params", warnings, [&](const ProtoField& g) {
                if (g.number == 0 || g.number >= PanoDewarpParams::SlotCount) {
                    noteUnknown("StreamMeta.pano_dewarp_params", g);
                    return;
                }
                if (!expectMessage(g, "StreamMeta.pano_dewarp_params", warnings)) {
                    return;
                }
                auto rec = decodeDewarp(g.bytes, warnings);
                if (!rec.ok()) {
                    addWarning(warnings, std::format("pano_dewarp_params slot {} ({}): {}", g.number,
                                                     PanoDewarpParams::fieldName(g.number), rec.error().toString()));
                    return;
                }
                s.dewarp.byField[g.number] = std::move(rec).value();
            });
            break;
        case 7:
            s.extriLensMode = static_cast<ExtriLensMode>(wrappedI32(f.bytes, "StreamMeta.extri_lens_mode", warnings));
            break;
        case 8:
            s.shadingCalibModeNum = wrappedU32(f.bytes, "StreamMeta.shading_calib_mode_num", warnings);
            break;
        default:
            // Reported under the number the camera wrote, not the canonical one.
            noteUnknown("StreamMeta", raw);
            return;
        }
        if (f.number < s.present.size()) {
            s.present.set(f.number);
        }
        ++decoded;
    });
    if (decoded == 0 && !clean) {
        return Error{ErrorCode::Malformed, "StreamMeta: no decodable field"};
    }
    // The Osmo 360 has always written its colour mode, so a missing one there
    // is unremarkable; on the Avata 360 it is worth a line, because the
    // histogram auto-detect then decides the whole colour path.
    if (schema == DjmdSchema::Avata360 && !s.present.test(4)) {
        addWarning(warnings, "Avata 360 colour mode not recorded (no StreamMeta camera_stream_meta.color_mode)");
    }
    return s;
}

// -----------------------------------------------------------------------------
//  FrameMeta
// -----------------------------------------------------------------------------

namespace {

/// DeviceAttitude {1 timestamp, 2 vsync, 3 repeated Quaternion, 4 offset}.
ImuBatch decodeImuBatch(ByteSpan body, const char* ctx, std::vector<std::string>* warnings) {
    ImuBatch b;
    b.present = true;
    scanMessage(body, ctx, warnings, [&](const ProtoField& f) {
        switch (f.number) {
        case 1: b.ts = f.asU32(); break;
        case 2: b.vsync = f.asU32(); break;
        case 3:
            if (expectMessage(f, ctx, warnings)) {
                b.q.push_back(DjmdDecoder::decodeQuaternion(f.bytes, warnings));
            }
            break;
        case 4: b.offset = f.asFloat(); break;
        default: noteUnknown(ctx, f); break;
        }
    });
    return b;
}

/// FrameMetaOfCamera (field 2 of FrameMeta), numbered by `map`.
void decodeCameraFrame(ByteSpan body, CameraFrame& cam, std::vector<std::string>* warnings, const FieldMap& map) {
    const char* ctx = "FrameMeta.camera_frame_meta";
    scanMessage(body, ctx, warnings, [&](const ProtoField& raw) {
        // Every field of this message is itself a message.
        if (!expectMessage(raw, ctx, warnings)) {
            return;
        }
        const ProtoField f = canonicalField(raw, map);
        switch (f.number) {
        case 1:
            // Device header (id/name/frequency of the camera device): unused.
            break;
        case 2: cam.exposureIndex = wrappedFloat(f.bytes, "camera.exposure_index", warnings); break;
        case 3: cam.iso = wrappedFloat(f.bytes, "camera.iso", warnings); break;
        case 4: cam.exposureTime = wrappedRepeatedI32(f.bytes, "camera.exposure_time", warnings); break;
        case 5: cam.digitalZoom = wrappedFloat(f.bytes, "camera.digital_zoom_ratio", warnings, 1.0f); break;
        case 6: cam.wbCct = wrappedU32(f.bytes, "camera.white_balance_cct", warnings); break;
        case 7: cam.orientation = wrappedI32(f.bytes, "camera.orientation", warnings); break;
        case 8:
            // underwater_confidence: not surfaced in the typed struct.
            break;
        case 9: cam.attitude = DjmdDecoder::decodeQuaternion(f.bytes, warnings); break;
        case 10:
            scanMessage(f.bytes, "camera.camera_acc", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 2: cam.acc.x = g.asFloat(); break;
                case 3: cam.acc.y = g.asFloat(); break;
                case 4: cam.acc.z = g.asFloat(); break;
                default: noteUnknown("camera.camera_acc", g); break;
                }
            });
            cam.accPresent = true;
            break;
        case 11: cam.sharpness = wrappedFloat(f.bytes, "camera.sharpness", warnings); break;
        case 12: cam.denoising = wrappedFloat(f.bytes, "camera.denoising", warnings); break;
        case 14:
            // track_ret: subject tracking result, not needed for stitching.
            break;
        case 15:
            scanMessage(f.bytes, "camera.aec_ae", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 1: cam.aecLv = g.asFloat(); break;
                case 2: cam.aecLuxIdx = g.asFloat(); break;
                case 3:
                case 4:
                case 5:
                    // Per-channel gains: not surfaced.
                    break;
                case 6: cam.aecAdrcGain = g.asFloat(); break;
                case 7:
                case 8:
                case 9:
                    // dark_boost, ae_mode, reserved: not surfaced.
                    break;
                default: noteUnknown("camera.aec_ae", g); break;
                }
            });
            break;
        case 16: cam.sensorTemperature = wrappedFloat(f.bytes, "camera.sensor_temperature", warnings); break;
        case 17: cam.eqFocal = wrappedRepeatedI32(f.bytes, "camera.equivalent_focal_length", warnings); break;
        default: noteUnknown(ctx, raw); break;
        }
    });
}

/// FrameMetaOfIMU (field 3 of FrameMeta).  Numbered identically on every
/// camera; the Avata 360's schema only appends fields 4 and 5.
void decodeImuFrame(ByteSpan body, ImuData& imu, std::vector<std::string>* warnings) {
    const char* ctx = "FrameMeta.imu_frame_meta";
    ImuBatch single;  // field 4, the fallback for a missing current batch
    scanMessage(body, ctx, warnings, [&](const ProtoField& f) {
        if (!expectMessage(f, ctx, warnings)) {
            return;
        }
        switch (f.number) {
        case 1:
            // Device header: unused.
            break;
        case 2:
            scanMessage(f.bytes, "imu.IMU_attitude_after_fusion", warnings, [&](const ProtoField& g) {
                if (!expectMessage(g, "imu.IMU_attitude_after_fusion", warnings)) {
                    return;
                }
                switch (g.number) {
                case 1: imu.current = decodeImuBatch(g.bytes, "imu.current", warnings); break;
                case 2: imu.prev = decodeImuBatch(g.bytes, "imu.prev", warnings); break;
                case 3: imu.next = decodeImuBatch(g.bytes, "imu.next", warnings); break;
                default: noteUnknown("imu.IMU_attitude_after_fusion", g); break;
                }
            });
            break;
        case 3: imu.vsyncPos = wrappedU32(f.bytes, "imu.IMU_vsync_pos", warnings); break;
        case 4:
            // IMU_single_attitude_after_fusion: a lone DeviceAttitude - the
            // very type of field 2's current_frame - that the Avata 360's
            // schema adds beside the three-batch message.  Kept aside and
            // used only when field 2 brings no current batch (below).
            single = decodeImuBatch(f.bytes, "imu.IMU_single_attitude_after_fusion", warnings);
            break;
        default: noteUnknown(ctx, f); break;
        }
    });
    if (!imu.current.present && single.present) {
        imu.current = std::move(single);
    }
}

/// FrameMetaOfGimbal (field 4 of FrameMeta): only the device header is kept.
void decodeGimbalFrame(ByteSpan body, FrameMeta& frame, std::vector<std::string>* warnings) {
    const char* ctx = "FrameMeta.gimbal_frame_meta";
    scanMessage(body, ctx, warnings, [&](const ProtoField& f) {
        if (!expectMessage(f, ctx, warnings)) {
            return;
        }
        switch (f.number) {
        case 1:
            scanMessage(f.bytes, "gimbal.dev_header", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 1:
                case 2:
                case 3:
                    // device_id and two enums: not surfaced.
                    break;
                case 4: frame.gimbalDeviceName = g.asString(); break;
                case 5: frame.gimbalDeviceFrequency = g.asFloat(); break;
                case 6:
                    // u64 reserved.
                    break;
                default: noteUnknown("gimbal.dev_header", g); break;
                }
            });
            break;
        case 2:
        case 3:
            // gps_basic / velocity: not present on the Osmo 360 clips seen.
            break;
        default: noteUnknown(ctx, f); break;
        }
    });
}

}  // namespace

Result<FrameMeta> DjmdDecoder::decodeFrameMeta(ByteSpan body, std::vector<std::string>* warnings,
                                               DjmdSchema schema) {
    FrameMeta frame;
    std::size_t decoded = 0;
    const SchemaMaps maps = schemaMaps(schema);
    const bool clean = scanMessage(body, "FrameMeta", warnings, [&](const ProtoField& raw) {
        if (!expectMessage(raw, "FrameMeta", warnings)) {
            return;
        }
        const ProtoField f = canonicalField(raw, maps.frameMeta);
        switch (f.number) {
        case 1:
            scanMessage(f.bytes, "FrameMeta.header", warnings, [&](const ProtoField& g) {
                switch (g.number) {
                case 1: frame.seq = g.asU32(); break;
                case 2: frame.timestampUs = g.asU64(); break;
                case 3: frame.streamId = g.asU32(); break;
                default:
                    // The header carries more fields (unknown meaning).
                    break;
                }
            });
            break;
        case 2: decodeCameraFrame(f.bytes, frame.camera, warnings, maps.cameraFrame); break;
        case 3: {
            ImuData imu;
            decodeImuFrame(f.bytes, imu, warnings);
            frame.imu = std::move(imu);
            break;
        }
        case 4: decodeGimbalFrame(f.bytes, frame, warnings); break;
        default:
            // Reported under the number the camera wrote, not the canonical one.
            noteUnknown("FrameMeta", raw);
            return;
        }
        ++decoded;
    });
    if (decoded == 0 && !clean) {
        return Error{ErrorCode::Malformed, "FrameMeta: no decodable field"};
    }
    return frame;
}

// -----------------------------------------------------------------------------
//  ProductMeta
// -----------------------------------------------------------------------------

Result<ProductMeta> DjmdDecoder::decode(ByteSpan sample, DjmdSchema schemaHint) {
    if (sample.empty()) {
        return Error{ErrorCode::InvalidArgument, "djmd sample is empty"};
    }
    ProductMeta product;
    product.schema = schemaHint;
    std::size_t decoded = 0;

    // ---- pass 1: ClipMeta, which names the schema ------------------------------
    // StreamMeta and FrameMeta are numbered by the schema, and nothing obliges
    // the camera to write ClipMeta first, so their bodies are only collected
    // here (in order: proto3 keeps the last good occurrence) and decoded once
    // the header has been read.  ProductMeta's own numbering (1 clip,
    // 2 stream, 3 frame) is the same on every camera.
    std::vector<ByteSpan> streamBodies;
    std::vector<ByteSpan> frameBodies;
    const bool clean = scanMessage(sample, "ProductMeta", &product.warnings, [&](const ProtoField& f) {
        if (!expectMessage(f, "ProductMeta", &product.warnings)) {
            return;
        }
        switch (f.number) {
        case 1: {
            auto clip = decodeClipMeta(f.bytes, &product.warnings);
            if (clip.ok()) {
                product.clip = std::move(clip).value();
                ++decoded;
            } else {
                addWarning(&product.warnings, clip.error().toString());
            }
            break;
        }
        case 2: streamBodies.push_back(f.bytes); break;
        case 3: frameBodies.push_back(f.bytes); break;
        default: noteUnknown("ProductMeta", f); break;
        }
    });
    // A sample that names its camera overrides the hint: sample 0 is where
    // the hint comes from in the first place.
    if (product.clip && !product.clip->header.protoFileName.empty()) {
        product.schema = djmdSchemaForProto(product.clip->header.protoFileName);
    }

    // ---- pass 2: StreamMeta and FrameMeta in the sample's schema ---------------
    for (const ByteSpan body : streamBodies) {
        auto stream = decodeStreamMeta(body, &product.warnings, product.schema);
        if (stream.ok()) {
            product.stream = std::move(stream).value();
            ++decoded;
        } else {
            addWarning(&product.warnings, stream.error().toString());
        }
    }
    for (const ByteSpan body : frameBodies) {
        auto frame = decodeFrameMeta(body, &product.warnings, product.schema);
        if (frame.ok()) {
            product.frame = std::move(frame).value();
            ++decoded;
        } else {
            addWarning(&product.warnings, frame.error().toString());
        }
    }
    if (decoded == 0) {
        // Nothing usable: report the scan failure if there was one, otherwise
        // the sample simply held no known message.
        return Error{ErrorCode::Malformed,
                     clean ? std::string("djmd sample holds no ClipMeta/StreamMeta/FrameMeta")
                           : (product.warnings.empty() ? std::string("djmd sample is malformed")
                                                       : product.warnings.back())};
    }
    return product;
}

}  // namespace osv::meta
