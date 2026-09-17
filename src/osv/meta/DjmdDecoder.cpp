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
    const bool clean = scanMessage(body, "ClipMeta", warnings, [&](const ProtoField& f) {
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
            noteUnknown("ClipMeta", f);
            return;
        }
        // Wrong wire type for a known wrapped field: say so once.
        if (!isMsg) {
            expectMessage(f, "ClipMeta", warnings);
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

Result<StreamMeta> DjmdDecoder::decodeStreamMeta(ByteSpan body, std::vector<std::string>* warnings) {
    StreamMeta s;
    std::size_t decoded = 0;
    const bool clean = scanMessage(body, "StreamMeta", warnings, [&](const ProtoField& f) {
        if (!expectMessage(f, "StreamMeta", warnings)) {
            return;
        }
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
            noteUnknown("StreamMeta", f);
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

/// FrameMetaOfCamera (field 2 of FrameMeta).
void decodeCameraFrame(ByteSpan body, CameraFrame& cam, std::vector<std::string>* warnings) {
    const char* ctx = "FrameMeta.camera_frame_meta";
    scanMessage(body, ctx, warnings, [&](const ProtoField& f) {
        // Every field of this message is itself a message.
        if (!expectMessage(f, ctx, warnings)) {
            return;
        }
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
        default: noteUnknown(ctx, f); break;
        }
    });
}

/// FrameMetaOfIMU (field 3 of FrameMeta).
void decodeImuFrame(ByteSpan body, ImuData& imu, std::vector<std::string>* warnings) {
    const char* ctx = "FrameMeta.imu_frame_meta";
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
        default: noteUnknown(ctx, f); break;
        }
    });
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

Result<FrameMeta> DjmdDecoder::decodeFrameMeta(ByteSpan body, std::vector<std::string>* warnings) {
    FrameMeta frame;
    std::size_t decoded = 0;
    const bool clean = scanMessage(body, "FrameMeta", warnings, [&](const ProtoField& f) {
        if (!expectMessage(f, "FrameMeta", warnings)) {
            return;
        }
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
        case 2: decodeCameraFrame(f.bytes, frame.camera, warnings); break;
        case 3: {
            ImuData imu;
            decodeImuFrame(f.bytes, imu, warnings);
            frame.imu = std::move(imu);
            break;
        }
        case 4: decodeGimbalFrame(f.bytes, frame, warnings); break;
        default:
            noteUnknown("FrameMeta", f);
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

Result<ProductMeta> DjmdDecoder::decode(ByteSpan sample) {
    if (sample.empty()) {
        return Error{ErrorCode::InvalidArgument, "djmd sample is empty"};
    }
    ProductMeta product;
    std::size_t decoded = 0;
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
        case 2: {
            auto stream = decodeStreamMeta(f.bytes, &product.warnings);
            if (stream.ok()) {
                product.stream = std::move(stream).value();
                ++decoded;
            } else {
                addWarning(&product.warnings, stream.error().toString());
            }
            break;
        }
        case 3: {
            auto frame = decodeFrameMeta(f.bytes, &product.warnings);
            if (frame.ok()) {
                product.frame = std::move(frame).value();
                ++decoded;
            } else {
                addWarning(&product.warnings, frame.error().toString());
            }
            break;
        }
        default: noteUnknown("ProductMeta", f); break;
        }
    });
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
