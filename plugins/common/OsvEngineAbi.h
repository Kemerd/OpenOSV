/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 The OpenOSV Contributors
 *
 * OsvEngineAbi.h - the C interface between the two OpenOSV plug-ins.
 *
 * WHY THIS EXISTS
 * ---------------
 * The direct GPU pipeline (docs/DIRECT_GPU.md) renders the reframed view
 * straight from the two fisheye frames, inside the effect, on Premiere's own
 * CUDA context.  Everything that knows about a CLIP - the container, the
 * calibration, the colour settings the user chose in Source Settings, the
 * stabilisation, the per-bucket stitch analyses, the hardware decoder and its
 * VRAM frame cache - already lives in the importer.  So the importer module
 * (OpenOSVImporter.prm) exports this small C ABI and the effect
 * (Open360Reframe.aex) calls it:
 *
 *     effect                                   importer ("engine")
 *     ------                                   -------------------
 *     find its source file + media time  --->  OsvEngine_AcquireFrame
 *                                              decode on NVDEC into the
 *                                              effect's CUcontext, run or
 *                                              fetch the analyses, upload the
 *                                              seam table / warp grid
 *                                        <---  device planes + stitch block
 *     fill the view fields, launch the
 *     fused kernel on its stream
 *     OsvEngine_ReleaseFrame(lease, stream) -> slot reusable once the stream
 *                                              has passed this point
 *
 * The effect never links the importer.  It finds the loaded module by name
 * and resolves these symbols with GetProcAddress, so the two plug-ins keep
 * independent builds and an absent or older importer is detected (version
 * and structure sizes are checked) and answered with the existing equirect
 * path instead of a crash.
 *
 * RULES
 * -----
 *  * Plain C: fixed-width integers, no C++ types, no exceptions across it.
 *  * Every struct starts with its own size, set by the caller, so either side
 *    can refuse a layout it was not built for.
 *  * OsvRenderParams / OsvPlane come from osv_kernel.h, which both modules
 *    compile from the same source tree; `paramsSize` guards against two
 *    builds from different trees meeting at runtime.
 *  * Nothing here allocates on the caller's behalf except the lease, which
 *    OsvEngine_ReleaseFrame always frees - including after a failed render.
 */
#ifndef OSV_ENGINE_ABI_H
#define OSV_ENGINE_ABI_H

#include "osv/render/osv_kernel.h"

#include <stdint.h>
#include <wchar.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bumped on ANY change to the structures or entry points below.
 *
 *  History:
 *    1  the first direct path (stitch block, planes, seam / warp tables).
 *    2  OsvEngineFrame gained the carved blend-seam table (WP-SEAM), the
 *       Source Settings the frame was rendered with (WP-SETTINGS) and the
 *       photometric seam field (WP-PHOTO); OsvEngine_QuerySettings was added;
 *       OsvRenderParams grew by the flare (WP-FLARE), look (WP-LOOK) and
 *       photo (WP-PHOTO) blocks.  A version-1 effect meeting a version-2 importer (or the
 *       reverse) refuses the engine and renders through the equirect path.
 *    3  OsvEngineFrame gained the seam smoothing's low band (WP-SEAMTOOLS,
 *       seamLowDevice) and OsvRenderParams its seam smoothing block; the
 *       direct kernel takes the low band as a new argument after the photo
 *       table.  A version-2 effect meeting a version-3 importer (or the
 *       reverse) refuses the engine, as above. */
#define OSV_ENGINE_ABI_VERSION 3u

/** Module file name of the importer that exports the engine. */
#define OSV_ENGINE_MODULE_NAME L"OpenOSVImporter.prm"

/** OsvEngineFrameRequest::purpose - mirrors the importer's RenderPurpose. */
#define OSV_ENGINE_PURPOSE_EXACT 0       /**< Paused frame / export: analyses must be final. */
#define OSV_ENGINE_PURPOSE_INTERACTIVE 1 /**< Playback / scrubbing: never wait for an analysis. */

/** OsvEngineFrameRequest::outputTransfer value meaning "the clip's own
 *  Source Settings choice" (any other value is an OSV_TRANSFER_* id from
 *  osv/color/ColorMath.h and overrides it). */
#define OSV_ENGINE_TRANSFER_FROM_CLIP (-1)

/** Status codes returned by OsvEngine_AcquireFrame. */
#define OSV_ENGINE_OK 0
#define OSV_ENGINE_ERR_ARGUMENT 1    /**< A null pointer, a bad size, a non-finite value. */
#define OSV_ENGINE_ERR_VERSION 2     /**< Structure sizes do not match this build. */
#define OSV_ENGINE_ERR_SOURCE 3      /**< The file cannot be opened as a dual-fisheye OSV. */
#define OSV_ENGINE_ERR_DECODE 4      /**< The frame could not be decoded on the GPU. */
#define OSV_ENGINE_ERR_GPU 5         /**< A CUDA call failed (context, memory, copy). */
#define OSV_ENGINE_ERR_INTERNAL 6    /**< Anything else; the message says what. */

/* ==== [WP-SETTINGS] the Source Settings a clip is rendered with ============
 *
 * WHY.  The direct path does not render from the frame Premiere hands the
 * effect; it renders from the fisheyes with the engine's OWN instance of the
 * file.  That instance learns the user's Source Settings from Premiere's
 * importer instances (they publish every change), so the effect needs to
 * know WHICH settings a frame was rendered with - to prove in the log that a
 * change reached the Program monitor, and to decide whether the direct path
 * can reproduce the colour the importer's equirect route would produce (see
 * docs/DIRECT_GPU.md, "WP-SETTINGS").
 *
 * The block is filled by OsvEngine_QuerySettings (cheap: no decode, no GPU)
 * and, for the frame actually served, by OsvEngine_AcquireFrame in
 * OsvEngineFrame::settings.
 * ------------------------------------------------------------------------- */

/** One file's Source Settings as the engine will render them. */
typedef struct OsvEngineClipSettings {
    uint32_t structSize;   /**< sizeof(OsvEngineClipSettings); set by the caller of the query, by the engine in a frame. */
    /** Bumped every time the settings in force for the file change.  0 means
     *  no Premiere importer instance of this file has published any in this
     *  process: the engine would render its defaults, which may not be what
     *  the clip is set to. */
    uint32_t generation;
    /** OSV_TRANSFER_* the clip's "colour output" encodes the importer's own
     *  frames in: what Premiere converts into the working space on the
     *  equirect route (OSV_TRANSFER_PASSTHROUGH for D-Log M passthrough). */
    int32_t clipTransfer;
    float exposureStops;   /**< Source Settings exposure offset, stops. */
    uint8_t colorOutput;   /**< The raw PrefsColorOutput value. */
    uint8_t calibration;   /**< The raw PrefsCalibration value. */
    uint8_t dlogmFit;      /**< The raw PrefsDlogmFit value. */
    uint8_t stabilization; /**< The raw PrefsStabilization value. */
    /** The raw PrefsDirectColour value ("Program Monitor Colour"): 0 =
     *  render every graded colour output straight into the working space
     *  (the default), 1 = hand a clip whose colour output is not the working
     *  space to the equirect route (match the Source monitor). */
    uint8_t directColour;
    uint8_t reservedSettings[3]; /**< Zero; keeps the 64-bit fields below naturally aligned. */
    /** Identity of the file the settings belong to (volume serial and file
     *  index, GetFileInformationByHandle); both 0 when the file could not be
     *  opened for its identity and the normalised path was used instead. */
    uint64_t fileVolume;
    uint64_t fileIndex;
} OsvEngineClipSettings;

/** Read the Source Settings the engine would render `path` with, without
 *  opening a decoder or touching the GPU.
 *  @param path   source media file, NUL-terminated UTF-16.
 *  @param out    receives the settings; out->structSize must be set.
 *  @param error  UTF-8 buffer for a one-line reason on failure (may be NULL).
 *  @param errorCapacity  bytes available in `error`.
 *  @return OSV_ENGINE_OK (also when nothing was published: generation 0) or
 *          OSV_ENGINE_ERR_ARGUMENT / OSV_ENGINE_ERR_VERSION / OSV_ENGINE_ERR_INTERNAL. */
typedef int32_t (*OsvEngineQuerySettingsFn)(const wchar_t* path, OsvEngineClipSettings* out, char* error,
                                            int32_t errorCapacity);

/** Exported name of the query (optional: a caller that cannot resolve it
 *  falls back to the settings reported with each frame). */
#define OSV_ENGINE_SYM_QUERY_SETTINGS "OsvEngine_QuerySettings"

/* ==== [/WP-SETTINGS] ====================================================== */

/** What the effect asks for. */
typedef struct OsvEngineFrameRequest {
    uint32_t structSize;    /**< sizeof(OsvEngineFrameRequest), set by the caller. */
    const wchar_t* path;    /**< Source media file, NUL-terminated UTF-16. */
    int64_t mediaTicks;     /**< Time from the start of the MEDIA, Premiere ticks (254016000000 per second). */
    int32_t purpose;        /**< OSV_ENGINE_PURPOSE_*. */
    int32_t outputTransfer; /**< OSV_ENGINE_TRANSFER_FROM_CLIP or an OSV_TRANSFER_* id. */
    void* cuContext;        /**< CUcontext the output frame lives in; every device pointer returned is valid in it. */
    void* cuStream;         /**< CUstream the caller will render on; the engine orders its uploads on it. */
} OsvEngineFrameRequest;

/** What the engine hands back: one frame of both lenses on the GPU plus the
 *  clip's stitch state.  Valid until OsvEngine_ReleaseFrame(lease, ...). */
typedef struct OsvEngineFrame {
    uint32_t structSize;          /**< sizeof(OsvEngineFrame), set by the CALLER before the call. */
    uint32_t paramsSize;          /**< sizeof(OsvRenderParams) the ENGINE was built with. */
    /** Lens, colour, blend, seam and warp fields filled for this frame.  The
     *  VIEW fields (outW/outH, mode, projection, focal, tan, eyeOffset, Rout)
     *  are left for the caller: it knows the view, the engine knows the clip. */
    OsvRenderParams stitch;
    OsvPlane planes[2];           /**< DEVICE pointers ([0] slave, [1] master), P010 interleaved. */
    const float* seamDevice;      /**< Device seam table (stitch.seamColumns floats) or NULL. */
    const float* warpDevice;      /**< Device warp grid (warpW * warpH * 2 floats) or NULL. */
    float bodyFromWorld[9];       /**< This frame's stabilisation, row-major; compose as Rout = bodyFromWorld * viewToWorld. */
    uint32_t frameIndex;          /**< The media frame that was decoded. */
    int32_t exact;                /**< 0 when an interactive request got a stand-in analysis. */
    void* lease;                  /**< Opaque; hand to OsvEngine_ReleaseFrame exactly once. */
    /* ---- [WP-SEAM] ---------------------------------------------------------- */
    /** Device copy of the carved blend-seam table (stitch.blendSeamColumns
     *  interleaved (latitude, half width) radian pairs, see osv_kernel.h) or
     *  NULL when stitch.blendSeamEnabled is 0.  Freed with the lease. */
    const float* blendSeamDevice;
    /* ---- [/WP-SEAM] --------------------------------------------------------- */
    /* ---- [WP-SETTINGS] ------------------------------------------------------ */
    /** The Source Settings THIS frame was rendered with (the engine sets
     *  settings.structSize).  generation 0: none were published for the file
     *  and the engine used its defaults. */
    OsvEngineClipSettings settings;
    /* ---- [/WP-SETTINGS] ----------------------------------------------------- */
    /* ---- [WP-PHOTO] --------------------------------------------------------- */
    /** Device copy of the photometric seam table (stitch.photoW * photoH * 3
     *  gain floats, then stitch.photoW * 2 rim floats, see osv_kernel.h) or
     *  NULL when stitch.photoEnabled is 0.  Freed with the lease. */
    const float* photoDevice;
    /* ---- [/WP-PHOTO] -------------------------------------------------------- */
    /* ---- [WP-SEAMTOOLS] ----------------------------------------------------- */
    /** Device copy of the seam smoothing's two-lens low band (2 x
     *  stitch.seamLowW x stitch.seamLowH RGBA float texels, see osv_kernel.h),
     *  built by the engine from THIS frame's planes on the request's stream,
     *  or NULL when stitch.seamSmoothEnabled is 0.  Freed with the lease. */
    const float* seamLowDevice;
    /* ---- [/WP-SEAMTOOLS] ---------------------------------------------------- */
} OsvEngineFrame;

/** ABI version of the loaded engine (compare with OSV_ENGINE_ABI_VERSION). */
typedef uint32_t (*OsvEngineAbiVersionFn)(void);

/** Decode + analyse one frame for the direct renderer.
 *  @param request  what to render (see OsvEngineFrameRequest).
 *  @param out      receives the frame; out->structSize must be set.
 *  @param error    UTF-8 buffer for a one-line reason on failure (may be NULL).
 *  @param errorCapacity  bytes available in `error`.
 *  @return OSV_ENGINE_OK or an OSV_ENGINE_ERR_* code.  On failure `out->lease`
 *          is NULL and nothing needs releasing. */
typedef int32_t (*OsvEngineAcquireFrameFn)(const OsvEngineFrameRequest* request, OsvEngineFrame* out, char* error,
                                           int32_t errorCapacity);

/** Give a frame back.  `cuStream` is the stream the caller queued its reads
 *  of the frame on: the slot is recycled only after the stream has passed
 *  this point, so the caller never has to synchronise.  NULL `lease` is a
 *  no-op; NULL `cuStream` releases immediately (only safe after a sync). */
typedef void (*OsvEngineReleaseFrameFn)(void* lease, void* cuStream);

/** Exported symbol names (resolved with GetProcAddress). */
#define OSV_ENGINE_SYM_ABI_VERSION "OsvEngine_AbiVersion"
#define OSV_ENGINE_SYM_ACQUIRE_FRAME "OsvEngine_AcquireFrame"
#define OSV_ENGINE_SYM_RELEASE_FRAME "OsvEngine_ReleaseFrame"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* OSV_ENGINE_ABI_H */
