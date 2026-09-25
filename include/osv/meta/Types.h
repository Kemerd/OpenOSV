// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Typed representation of the DJI "dvtm" per-frame metadata carried in the
// `djmd` tracks of an .OSV file.  Field numbers are documented next to every
// member so the decoder (DjmdDecoder) and the schema transcription in
// proto/dvtm_osmo360.proto stay in sync.  Everything is optional: a missing
// field keeps its default and the `present` bit stays clear.
//
// Coordinate conventions carried by these values are documented in
// docs/GEOMETRY.md (calibration is in 3840 x 3840 sensor pixels; quaternions
// are stored (w, x, y, z)).
#pragma once

#include "osv/core/ByteSpan.h"
#include "osv/core/Math.h"

#include <array>
#include <bitset>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace osv::meta {

// -----------------------------------------------------------------------------
//  Enumerations (values are the protobuf enum numbers written by the camera)
// -----------------------------------------------------------------------------

/// StreamMeta.color_mode (library ColorMode.ColorModeType).
enum class ColorMode : std::int32_t {
    Normal = 0,      ///< COLOR_MODE_DEFAULT
    DCinelike = 1,   ///< COLOR_MODE_CAM_MLGLIKE
    DLog = 2,        ///< COLOR_MODE_CAM_MLG
    HLG = 9,         ///< COLOR_MODE_HLG
    Vivid = 12,      ///< COLOR_MODE_VIVID
    DLogM = 19,      ///< COLOR_MODE_CAM_MLG_M  (the mode this project targets)
    DLog2 = 22,      ///< COLOR_MODE_CAM_MLG2
    Unknown = -1     ///< Not present in the file.
};

/// StreamMeta.extri_lens_mode (which accessory calibration set applies).
enum class ExtriLensMode : std::int32_t {
    Native = 0,      ///< EXTRI_LENS_MODE_NATIVE_REFINE
    LensGuards = 1,  ///< EXTRI_LENS_MODE_LENS_GUARDS
    Underwater = 2   ///< EXTRI_LENS_MODE_WATER_PROOF_UNDER_WATER
};

/// ClipMeta.eis_status (in-camera electronic stabilisation state).
enum class EisStatus : std::int32_t {
    Off = 0,
    RockSteady = 1,
    HorizonSteady = 2,
    Hyper = 3,
    Tradeoff = 4,
    HorizonBalancing = 5,
    DeepSpace = 6,
    OffWithCrop = 7,
    HorizonCorrection = 8,
    RsAuto = 9
};

/// Human readable names for the enums above (for JSON / probe output).
[[nodiscard]] const char* colorModeName(ColorMode mode) noexcept;
[[nodiscard]] const char* extriLensModeName(ExtriLensMode mode) noexcept;
[[nodiscard]] const char* eisStatusName(EisStatus status) noexcept;

// -----------------------------------------------------------------------------
//  Quaternion as stored by the camera: message Quaternion {1 w, 2 x, 3 y, 4 z}
// -----------------------------------------------------------------------------
struct Quaternion {
    float w = 1.0f;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    bool present = false;  ///< True when the message was found in the file.

    /// Convert taking the stored order at face value (w, x, y, z).
    [[nodiscard]] Quatd toQuatdWXYZ() const noexcept { return Quatd::fromWXYZ(w, x, y, z); }
    /// Convert interpreting the four stored floats as (x, y, z, w).
    [[nodiscard]] Quatd toQuatdXYZW() const noexcept { return Quatd::fromXYZW(w, x, y, z); }
};

// -----------------------------------------------------------------------------
//  DewarpParams: one lens calibration record (library message DewarpParams)
// -----------------------------------------------------------------------------
struct DewarpParams {
    float fx = 0.0f;                    ///< 1  focal length x (px, calibration frame)
    float fy = 0.0f;                    ///< 2  focal length y
    float cx = 0.0f;                    ///< 3  principal point x
    float cy = 0.0f;                    ///< 4  principal point y
    std::array<float, 9> k{};           ///< 5-8 k1..k4, 15-19 k5..k9 (radial polynomial)
    float xi = 0.0f;                    ///< 9  unified-model parameter (unused: 0)
    std::uint32_t width = 0;            ///< 10 calibration frame width (3840)
    std::uint32_t height = 0;           ///< 11 calibration frame height (3840)
    float yaw = 0.0f;                   ///< 12 Euler yaw   (deg, informational only)
    float pitch = 0.0f;                 ///< 13 Euler pitch (deg, informational only)
    float roll = 0.0f;                  ///< 14 Euler roll  (deg, informational only)
    std::vector<float> p;               ///< 20 tangential terms (p1, p2)
    std::vector<float> q;               ///< 21 quaternion as 4 floats (copy of cam_extri_q)
    std::vector<float> occlusionPtX;    ///< 22 occlusion polygon x (calibration px)
    std::vector<float> occlusionPtY;    ///< 23 occlusion polygon y
    float lensModel = 0.0f;             ///< 24 lens model id (8.0 on Osmo 360)
    float temperature = 0.0f;           ///< 25 calibration temperature (-1000 = unset)
    Quaternion camImuExtriQ;            ///< 26 camera->IMU quaternion (absent on Osmo 360)
    std::vector<float> tangentCoeff;    ///< 27 tangential coefficients (duplicate of p)
    Quaternion camExtriQ;               ///< 28 body->lens quaternion (w,x,y,z)
    bool camImuCaliEnable = false;      ///< 29
    bool tempCompenEnable = false;      ///< 30
    float tempCompenK = 0.0f;           ///< 31
    float gimbalYawH1 = 0.0f;           ///< 32
    float gimbalYawH2 = 0.0f;           ///< 33
    std::vector<float> tempCompenKOrder;///< 35
    std::bitset<64> present;            ///< Bit n set when proto field n was decoded.

    /// True when the fields needed for projection are all present and sane
    /// (fx, fy, cx, cy > 0, width/height > 0, extrinsic quaternion present).
    [[nodiscard]] bool hasCore() const noexcept;

    /// True when every numeric field is zero (an empty slot in the file).
    [[nodiscard]] bool isEmpty() const noexcept;
};

/// PanoDewarpParams: 24 calibration slots addressed by proto field number.
struct PanoDewarpParams {
    /// Field numbers of the slots (1-based, index 0 unused).
    enum Slot : std::uint32_t {
        NativeRefineSlave = 1,
        NativeRefineMaster = 2,
        NativeRefineFarSlave = 3,
        NativeRefineFarMaster = 4,
        LensGuardsSlave = 5,
        LensGuardsMaster = 6,
        WaterAboveSlave = 7,
        WaterAboveMaster = 8,
        WaterUnderSlave = 9,
        WaterUnderMaster = 10,
        NativeSlave = 11,
        NativeMaster = 12,
        Far07Slave = 13,
        Far07Master = 14,
        Far09Slave = 15,
        Far09Master = 16,
        Far11Slave = 17,
        Far11Master = 18,
        Far12_5Slave = 19,
        Far12_5Master = 20,
        Far14Slave = 21,
        Far14Master = 22,
        Far16Slave = 23,
        Far16Master = 24,
        SlotCount = 25
    };

    std::array<std::optional<DewarpParams>, SlotCount> byField;

    /// Stable proto field name for a slot ("native_refine_slave", ...), or
    /// "unknown" for out-of-range numbers.
    [[nodiscard]] static const char* fieldName(std::uint32_t field) noexcept;

    /// Pointer to the record for `field`, or nullptr when absent/empty.
    [[nodiscard]] const DewarpParams* get(std::uint32_t field) const noexcept;
};

// -----------------------------------------------------------------------------
//  ClipMeta (per clip; only in djmd sample 0)
// -----------------------------------------------------------------------------
struct ClipHeader {
    std::string protoFileName;        ///< 1  "dvtm_oq101.proto"
    std::string libVersion;           ///< 2  "02.01.15"
    std::string productProtoVersion;  ///< 3  "2.0.8"
    std::string serialNumber;         ///< 5  camera serial
    std::string firmware;             ///< 6  "10.00.25.29"
    std::uint64_t clipTimestampUs = 0;///< 9  us since power-up at first frame
    std::string productName;          ///< 10 "Osmo 360"
};

struct ClipMeta {
    ClipHeader header;                          ///< 1
    std::uint32_t videoStreamCount = 0;         ///< 2.1
    std::uint32_t audioStreamCount = 0;         ///< 2.2
    std::vector<float> distortionCoefficients;  ///< 3.1 (flat-lens OpenCV fisheye k1..k4)
    std::uint64_t sensorReadoutTime = 0;        ///< 4.1 (units unverified, ~17282160)
    std::int32_t sensorReadDirection = 0;       ///< 5.1 (4 = LEFT_TOP)
    float digitalFocalLength = 0.0f;            ///< 8.1 stream-space focal length (829.3612 @ 6K)
    EisStatus eisStatus = EisStatus::Off;       ///< 9.1
    std::uint32_t imuSamplingRate = 0;          ///< 10.1 (1000 Hz)
    float sensorFps = 0.0f;                     ///< 11.1
    std::int32_t itd = 0;                       ///< 12.1
    std::uint32_t lro = 0;                      ///< 13.1
    std::uint32_t sensorW = 0;                  ///< 14.1 (3840)
    std::uint32_t sensorH = 0;                  ///< 14.2 (3840)
    std::vector<std::uint32_t> fNumber;         ///< 15.1 rational [19,10] = f/1.9
    std::uint32_t styleFilterMode = 0;          ///< 16.1
    std::uint32_t yltmEnable = 0;               ///< 17.1
    std::bitset<32> present;                    ///< Bit n set when top-level field n decoded.
};

// -----------------------------------------------------------------------------
//  StreamMeta (per video stream; only in djmd sample 0)
// -----------------------------------------------------------------------------
struct VideoInfo {
    std::uint32_t width = 0;        ///< 1
    std::uint32_t height = 0;       ///< 2
    float fps = 0.0f;               ///< 3
    bool bitDepthValid = false;     ///< 4
    std::uint32_t bitDepth = 0;     ///< 5 (10)
    std::int32_t bitFormat = 0;     ///< 6 (4 = YUV420)
    std::int32_t streamType = 0;    ///< 7
    std::int32_t codec = 0;         ///< 8 (1 = H265)
};

struct StreamMeta {
    std::uint32_t id = 0;                                   ///< 1.1
    std::int32_t type = 0;                                  ///< 1.2 (0 video, 1 audio)
    std::string name;                                       ///< 1.3 ("video")
    VideoInfo video;                                        ///< 3
    ColorMode colorMode = ColorMode::Unknown;               ///< 4.1 (2.4.1 on the Avata 360, see DjmdDecoder)
    std::int32_t fovType = 0;                               ///< 5.1 (3 = WIDE)
    PanoDewarpParams dewarp;                                ///< 6
    ExtriLensMode extriLensMode = ExtriLensMode::Native;    ///< 7.1
    std::uint32_t shadingCalibModeNum = 0;                  ///< 8.1
    std::bitset<32> present;
};

// -----------------------------------------------------------------------------
//  FrameMeta (every djmd sample)
// -----------------------------------------------------------------------------

/// DeviceAttitude: one batch of fused IMU orientation samples.
struct ImuBatch {
    std::uint32_t ts = 0;         ///< 1 first sample timestamp (attitude clock ticks)
    std::uint32_t vsync = 0;      ///< 2 sensor frame counter
    std::vector<Quaternion> q;    ///< 3 ~16-17 unit quaternions per 59.94 fps frame
    float offset = 0.0f;          ///< 4 (-0.9025 constant on the sample clip)
    bool present = false;
};

/// FrameMetaOfIMU.
struct ImuData {
    ImuBatch current;             ///< 2.1
    ImuBatch prev;                ///< 2.2 (absent on Osmo 360)
    ImuBatch next;                ///< 2.3 (absent on Osmo 360)
    std::uint32_t vsyncPos = 0;   ///< 3.1 (0..7 cycling phase)
};

/// FrameMetaOfCamera (subset we keep; the rest is retained raw for probe).
struct CameraFrame {
    float exposureIndex = 0.0f;            ///< 2.1
    float iso = 0.0f;                      ///< 3.1
    std::vector<std::int32_t> exposureTime;///< 4.1 rational [num, den] seconds
    float digitalZoom = 1.0f;              ///< 5.1
    std::uint32_t wbCct = 0;               ///< 6.1 white balance (K)
    std::int32_t orientation = 0;          ///< 7.1
    Quaternion attitude;                   ///< 9   camera attitude (== imu.current.q[4])
    Vec3f acc{};                           ///< 10  (fields 2,3,4) motion vector, units unknown
    bool accPresent = false;
    float sharpness = 0.0f;                ///< 11.1
    float denoising = 0.0f;                ///< 12.1
    float aecLv = 0.0f;                    ///< 15.1 light value
    float aecLuxIdx = 0.0f;                ///< 15.2
    float aecAdrcGain = 0.0f;              ///< 15.6
    float sensorTemperature = 0.0f;        ///< 16.1
    std::vector<std::int32_t> eqFocal;     ///< 17.1
};

struct FrameMeta {
    std::uint32_t seq = 0;            ///< 1.1 (+2 per frame on the OSV)
    std::uint64_t timestampUs = 0;    ///< 1.2 (us since power-up)
    std::uint32_t streamId = 0;       ///< 1.3
    CameraFrame camera;               ///< 2
    std::optional<ImuData> imu;       ///< 3 (only on the primary djmd track)
    std::string gimbalDeviceName;     ///< 4.1.4 ("Osmo OQ001")
    float gimbalDeviceFrequency = 0;  ///< 4.1.5
};

/// One decoded djmd sample: ProductMeta {1 clip, 2 stream, 3 frame}.
struct ProductMeta {
    std::optional<ClipMeta> clip;
    std::optional<StreamMeta> stream;
    std::optional<FrameMeta> frame;
    std::vector<std::string> warnings;  ///< Non-fatal decode problems.
};

/// The pair of lens calibrations selected for stitching.
struct CalibrationSet {
    DewarpParams slave;        ///< Lens of video track 1 / stream 0 (optical axis -Y body).
    DewarpParams master;       ///< Lens of video track 2 / stream 1 (optical axis +Y body).
    std::string sourceSlave;   ///< Slot name the slave record came from.
    std::string sourceMaster;  ///< Slot name the master record came from.
};

}  // namespace osv::meta
