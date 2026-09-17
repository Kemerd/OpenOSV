// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// JSON serialisation of the metadata structs.  Key names are shared with
// scripts/gen_golden.py; change both when renaming anything.

#include "osv/meta/MetaJson.h"

#include "osv/core/Log.h"

#include <cstddef>
#include <string>

namespace osv::meta {

using nlohmann::json;

namespace {

/// Float vector -> JSON array of doubles (exact widening).
json floatArray(const std::vector<float>& v) {
    json a = json::array();
    for (const float f : v) {
        a.push_back(static_cast<double>(f));
    }
    return a;
}

/// Bitset -> sorted array of the set bit indices.
template <std::size_t N>
json presentArray(const std::bitset<N>& bits) {
    json a = json::array();
    for (std::size_t i = 0; i < N; ++i) {
        if (bits.test(i)) {
            a.push_back(static_cast<std::uint32_t>(i));
        }
    }
    return a;
}

/// Strings from the file may hold anything; keep the JSON valid UTF-8 by
/// replacing bytes that are not 7-bit ASCII (log::safe does exactly that).
json safeString(const std::string& s) { return log::safe(s); }

}  // namespace

json toJson(const Quaternion& q) {
    if (!q.present) {
        return nullptr;
    }
    return json{{"w", static_cast<double>(q.w)},
                {"x", static_cast<double>(q.x)},
                {"y", static_cast<double>(q.y)},
                {"z", static_cast<double>(q.z)}};
}

json toJson(const DewarpParams& d) {
    json k = json::array();
    for (const float v : d.k) {
        k.push_back(static_cast<double>(v));
    }
    json j;
    j["fx"] = static_cast<double>(d.fx);
    j["fy"] = static_cast<double>(d.fy);
    j["cx"] = static_cast<double>(d.cx);
    j["cy"] = static_cast<double>(d.cy);
    j["k"] = k;
    j["xi"] = static_cast<double>(d.xi);
    j["width"] = d.width;
    j["height"] = d.height;
    j["yaw"] = static_cast<double>(d.yaw);
    j["pitch"] = static_cast<double>(d.pitch);
    j["roll"] = static_cast<double>(d.roll);
    j["p"] = floatArray(d.p);
    j["q"] = floatArray(d.q);
    j["occlusionPtX"] = floatArray(d.occlusionPtX);
    j["occlusionPtY"] = floatArray(d.occlusionPtY);
    j["lensModel"] = static_cast<double>(d.lensModel);
    j["temperature"] = static_cast<double>(d.temperature);
    j["camImuExtriQ"] = toJson(d.camImuExtriQ);
    j["tangentCoeff"] = floatArray(d.tangentCoeff);
    j["camExtriQ"] = toJson(d.camExtriQ);
    j["camImuCaliEnable"] = d.camImuCaliEnable;
    j["tempCompenEnable"] = d.tempCompenEnable;
    j["tempCompenK"] = static_cast<double>(d.tempCompenK);
    j["gimbalYawH1"] = static_cast<double>(d.gimbalYawH1);
    j["gimbalYawH2"] = static_cast<double>(d.gimbalYawH2);
    j["tempCompenKOrder"] = floatArray(d.tempCompenKOrder);
    j["presentFields"] = presentArray(d.present);
    j["hasCore"] = d.hasCore();
    return j;
}

json toJson(const PanoDewarpParams& p) {
    json j = json::object();
    // Every slot appears, populated or null, so consumers can rely on the keys.
    for (std::uint32_t slot = 1; slot < PanoDewarpParams::SlotCount; ++slot) {
        const DewarpParams* rec = p.get(slot);
        j[PanoDewarpParams::fieldName(slot)] = rec ? toJson(*rec) : json(nullptr);
    }
    return j;
}

json toJson(const ClipMeta& c) {
    json header;
    header["protoFileName"] = safeString(c.header.protoFileName);
    header["libVersion"] = safeString(c.header.libVersion);
    header["productProtoVersion"] = safeString(c.header.productProtoVersion);
    header["serialNumber"] = safeString(c.header.serialNumber);
    header["firmware"] = safeString(c.header.firmware);
    header["clipTimestampUs"] = c.header.clipTimestampUs;
    header["productName"] = safeString(c.header.productName);

    json j;
    j["header"] = header;
    j["videoStreamCount"] = c.videoStreamCount;
    j["audioStreamCount"] = c.audioStreamCount;
    j["distortionCoefficients"] = floatArray(c.distortionCoefficients);
    j["sensorReadoutTime"] = c.sensorReadoutTime;
    j["sensorReadDirection"] = c.sensorReadDirection;
    j["digitalFocalLength"] = static_cast<double>(c.digitalFocalLength);
    j["eisStatus"] = static_cast<std::int32_t>(c.eisStatus);
    j["eisStatusName"] = eisStatusName(c.eisStatus);
    j["imuSamplingRate"] = c.imuSamplingRate;
    j["sensorFps"] = static_cast<double>(c.sensorFps);
    j["itd"] = c.itd;
    j["lro"] = c.lro;
    j["sensorW"] = c.sensorW;
    j["sensorH"] = c.sensorH;
    j["fNumber"] = c.fNumber;
    j["styleFilterMode"] = c.styleFilterMode;
    j["yltmEnable"] = c.yltmEnable;
    j["presentFields"] = presentArray(c.present);
    return j;
}

json toJson(const StreamMeta& s) {
    json video;
    video["width"] = s.video.width;
    video["height"] = s.video.height;
    video["fps"] = static_cast<double>(s.video.fps);
    video["bitDepthValid"] = s.video.bitDepthValid;
    video["bitDepth"] = s.video.bitDepth;
    video["bitFormat"] = s.video.bitFormat;
    video["streamType"] = s.video.streamType;
    video["codec"] = s.video.codec;

    json j;
    j["id"] = s.id;
    j["type"] = s.type;
    j["name"] = safeString(s.name);
    j["video"] = video;
    j["colorMode"] = static_cast<std::int32_t>(s.colorMode);
    j["colorModeName"] = colorModeName(s.colorMode);
    j["fovType"] = s.fovType;
    j["extriLensMode"] = static_cast<std::int32_t>(s.extriLensMode);
    j["extriLensModeName"] = extriLensModeName(s.extriLensMode);
    j["shadingCalibModeNum"] = s.shadingCalibModeNum;
    j["dewarp"] = toJson(s.dewarp);
    j["presentFields"] = presentArray(s.present);
    return j;
}

json toJson(const ImuBatch& b) {
    if (!b.present) {
        return nullptr;
    }
    json q = json::array();
    for (const Quaternion& item : b.q) {
        q.push_back(toJson(item));
    }
    json j;
    j["ts"] = b.ts;
    j["vsync"] = b.vsync;
    j["offset"] = static_cast<double>(b.offset);
    j["count"] = static_cast<std::uint32_t>(b.q.size());
    j["first"] = b.q.empty() ? json(nullptr) : toJson(b.q.front());
    j["last"] = b.q.empty() ? json(nullptr) : toJson(b.q.back());
    j["anchor4"] = b.q.size() > 4 ? toJson(b.q[4]) : json(nullptr);
    j["q"] = q;
    return j;
}

json toJson(const FrameMeta& f) {
    json camera;
    camera["exposureIndex"] = static_cast<double>(f.camera.exposureIndex);
    camera["iso"] = static_cast<double>(f.camera.iso);
    camera["exposureTime"] = f.camera.exposureTime;
    camera["digitalZoom"] = static_cast<double>(f.camera.digitalZoom);
    camera["wbCct"] = f.camera.wbCct;
    camera["orientation"] = f.camera.orientation;
    camera["attitude"] = toJson(f.camera.attitude);
    if (f.camera.accPresent) {
        camera["acc"] = json{{"x", static_cast<double>(f.camera.acc.x)},
                             {"y", static_cast<double>(f.camera.acc.y)},
                             {"z", static_cast<double>(f.camera.acc.z)}};
    } else {
        camera["acc"] = nullptr;
    }
    camera["sharpness"] = static_cast<double>(f.camera.sharpness);
    camera["denoising"] = static_cast<double>(f.camera.denoising);
    camera["aecLv"] = static_cast<double>(f.camera.aecLv);
    camera["aecLuxIdx"] = static_cast<double>(f.camera.aecLuxIdx);
    camera["aecAdrcGain"] = static_cast<double>(f.camera.aecAdrcGain);
    camera["sensorTemperature"] = static_cast<double>(f.camera.sensorTemperature);
    camera["eqFocal"] = f.camera.eqFocal;

    json j;
    j["seq"] = f.seq;
    j["timestampUs"] = f.timestampUs;
    j["streamId"] = f.streamId;
    j["camera"] = camera;
    if (f.imu.has_value()) {
        json imu;
        imu["current"] = toJson(f.imu->current);
        imu["prev"] = toJson(f.imu->prev);
        imu["next"] = toJson(f.imu->next);
        imu["vsyncPos"] = f.imu->vsyncPos;
        j["imu"] = imu;
    } else {
        j["imu"] = nullptr;
    }
    j["gimbalDeviceName"] = safeString(f.gimbalDeviceName);
    j["gimbalDeviceFrequency"] = static_cast<double>(f.gimbalDeviceFrequency);
    return j;
}

json toJson(const ProductMeta& p) {
    json j;
    j["clip"] = p.clip ? toJson(*p.clip) : json(nullptr);
    j["stream"] = p.stream ? toJson(*p.stream) : json(nullptr);
    j["frame"] = p.frame ? toJson(*p.frame) : json(nullptr);
    json warnings = json::array();
    for (const std::string& w : p.warnings) {
        warnings.push_back(safeString(w));
    }
    j["warnings"] = warnings;
    return j;
}

json toJson(const FormatInfo& f) {
    json j;
    j["cameraModel"] = safeString(f.cameraModel);
    j["mode"] = modeName(f.mode);
    j["modeValue"] = static_cast<std::int32_t>(f.mode);
    j["streamW"] = f.streamW;
    j["streamH"] = f.streamH;
    j["fps"] = f.fps;
    j["colorMode"] = static_cast<std::int32_t>(f.colorMode);
    j["colorModeName"] = colorModeName(f.colorMode);
    j["colorModeFromMetadata"] = f.colorModeFromMetadata;
    j["lensMode"] = static_cast<std::int32_t>(f.lensMode);
    j["lensModeName"] = extriLensModeName(f.lensMode);
    j["bitDepth"] = f.bitDepth;
    j["dualFisheye"] = f.dualFisheye;
    j["videoTrackIds"] = json::array({f.videoTrackIds[0], f.videoTrackIds[1]});
    j["metaTrackId"] = f.metaTrackId;
    j["sensorW"] = f.sensorW;
    j["sensorH"] = f.sensorH;
    j["digitalFocalLength"] = static_cast<double>(f.digitalFocalLength);
    j["sideBySideProxy"] = f.sideBySideProxy;
    json notes = json::array();
    for (const std::string& n : f.notes) {
        notes.push_back(safeString(n));
    }
    j["notes"] = notes;
    return j;
}

json toJson(const CalibrationSet& c) {
    json j;
    j["sourceSlave"] = c.sourceSlave;
    j["sourceMaster"] = c.sourceMaster;
    j["slave"] = toJson(c.slave);
    j["master"] = toJson(c.master);
    return j;
}

json toJson(const ProtoNode& n) {
    json j;
    j["field"] = n.number;
    j["wire"] = wireTypeName(n.wire);
    switch (n.wire) {
    case WireType::Varint:
        j["varint"] = n.varint;
        // The signed reading is handy when the value is a negative int32.
        j["int32"] = static_cast<std::int32_t>(static_cast<std::uint32_t>(n.varint & 0xFFFFFFFFu));
        break;
    case WireType::Fixed32: {
        ProtoField tmp;
        tmp.wire = WireType::Fixed32;
        tmp.fixed32 = n.fixed32;
        j["fixed32"] = n.fixed32;
        j["float"] = static_cast<double>(tmp.asFloat());
        break;
    }
    case WireType::Fixed64: {
        ProtoField tmp;
        tmp.wire = WireType::Fixed64;
        tmp.fixed64 = n.fixed64;
        j["fixed64"] = n.fixed64;
        j["double"] = tmp.asDouble();
        break;
    }
    case WireType::LengthDelimited:
        j["length"] = static_cast<std::uint64_t>(n.bytes.size());
        if (n.isMessage) {
            json children = json::array();
            for (const ProtoNode& child : n.children) {
                children.push_back(toJson(child));
            }
            j["children"] = children;
            if (n.truncated) {
                j["truncated"] = true;
                j["failure"] = n.failure;
            }
        } else if (n.isPrintableAscii()) {
            j["string"] = std::string(reinterpret_cast<const char*>(n.bytes.data()), n.bytes.size());
        } else {
            // Hex dump, capped so a huge blob does not explode the document.
            static constexpr std::size_t kMaxHex = 256;
            static constexpr char kDigits[] = "0123456789abcdef";
            std::string hex;
            const std::size_t count = n.bytes.size() < kMaxHex ? n.bytes.size() : kMaxHex;
            hex.reserve(count * 2);
            for (std::size_t i = 0; i < count; ++i) {
                hex.push_back(kDigits[n.bytes[i] >> 4]);
                hex.push_back(kDigits[n.bytes[i] & 0xF]);
            }
            j["hex"] = hex;
            if (n.bytes.size() > kMaxHex) {
                j["hexTruncated"] = true;
            }
            // Packed float arrays are common in this schema; show them too.
            if (!n.bytes.empty() && n.bytes.size() % 4 == 0 && n.bytes.size() <= 4096) {
                json floats = json::array();
                ByteReader reader(ByteSpan(n.bytes));
                float f = 0.0f;
                while (reader.f32le(f)) {
                    floats.push_back(static_cast<double>(f));
                }
                j["asFloats"] = floats;
            }
        }
        break;
    case WireType::StartGroup:
    case WireType::EndGroup:
        break;
    }
    return j;
}

json toJson(const ProtoTree& t) {
    json fields = json::array();
    for (const ProtoNode& n : t.fields) {
        fields.push_back(toJson(n));
    }
    json j;
    j["fields"] = fields;
    j["failed"] = t.failed;
    j["failure"] = t.failure;
    j["nodeCount"] = static_cast<std::uint64_t>(t.nodeCount);
    return j;
}

}  // namespace osv::meta
