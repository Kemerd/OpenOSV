// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OfxIds.cs - the OpenFX identity of OpenOSV Source and OpenOSV 360 Reframe,
// as C# constants.
//
// ===========================================================================
//  One source of truth, checked
// ===========================================================================
// The names below are PERMANENT: VEGAS (like DaVinci Resolve) stores them in
// every project.  They are defined in C++:
//
//   plugins/ofx/OfxSource.h         the generator's id, file / output / start
//   plugins/ofx/OfxSourceParams.h   the stitching and colour controls
//   plugins/ofx/OfxCamera.h         the camera controls, shared by both effects
//   plugins/ofx/OfxReframe.h        the filter's id
//   plugins/sourcesettings/SourceSettingsParams.h   the popup item strings
//   plugins/reframe/ReframeParams.h                 preset / lens / easing items
//
// and repeated here because C# cannot include a header.  The test
// HeaderContractTests parses those files at test time and compares every
// constant, so a renamed parameter or a reordered popup fails the build's
// tests instead of shipping an extension that writes to nothing.

using System;
using System.Collections.Generic;

namespace OpenOSV.Vegas.Core
{
    /// <summary>The two OpenFX plug-ins and how VEGAS lists them.</summary>
    public static class OfxIds
    {
        /// <summary>OpenOSV Source's OpenFX identifier (OfxSource.h, kPluginId).</summary>
        public const string SourcePluginId = "org.openosv.OSVSource";

        /// <summary>OpenOSV 360 Reframe's OpenFX identifier (OfxReframe.h, kPluginId).</summary>
        public const string ReframePluginId = "org.openosv.Open360Reframe";

        /// <summary>
        /// How the VEGAS scripting API names an OpenFX plug-in in its
        /// <c>PlugInNode.UniqueID</c>: <c>{Svfx:&lt;identifier&gt;}</c>.
        /// </summary>
        public static string VegasUniqueId(string ofxIdentifier) => "{Svfx:" + (ofxIdentifier ?? string.Empty) + "}";

        /// <summary><c>{Svfx:org.openosv.OSVSource}</c>.</summary>
        public static readonly string SourceUniqueId = VegasUniqueId(SourcePluginId);

        /// <summary><c>{Svfx:org.openosv.Open360Reframe}</c>.</summary>
        public static readonly string ReframeUniqueId = VegasUniqueId(ReframePluginId);

        /// <summary>The generator's label (OfxSource.cpp, kOfxPropLabel).</summary>
        public const string SourceLabel = "OpenOSV Source";

        /// <summary>The filter's label.</summary>
        public const string ReframeLabel = "OpenOSV 360 Reframe";
    }

    /// <summary>OpenOSV Source's own parameters (OfxSource.h).</summary>
    public static class SourceParams
    {
        /// <summary>The .OSV / .LRF path (string, file path).</summary>
        public const string File = "file";
        /// <summary>The Choose .OSV File button.</summary>
        public const string ChooseFile = "chooseFile";
        /// <summary>The Clip read-out (string label).</summary>
        public const string ClipInfo = "clipInfo";
        /// <summary>Reframed view | 360 equirect (choice).</summary>
        public const string Output = "output";
        /// <summary>The clip frame on the generator's first frame (integer).</summary>
        public const string StartFrame = "startFrame";
        /// <summary>
        /// Full range | Studio RGB (choice).  Defined only when the host is
        /// VEGAS (WP-V-OFX); every write to it tolerates its absence.
        /// </summary>
        public const string OutputLevels = "outputLevels";
    }

    /// <summary>The stitching and colour controls (OfxSourceParams.h).</summary>
    public static class StitchParams
    {
        public const string ColourGroup = "colourGroup";
        public const string ColorOutput = "colorOutput";
        public const string HdrTone = "hdrTone";
        public const string Look = "look";
        public const string HdrPeak = "hdrPeak";
        public const string Stabilization = "stabilization";
        public const string StitchGroup = "stitchGroup";
        public const string SeamSearch = "seamSearch";
        public const string GainMatch = "gainMatch";
        public const string Calibration = "calibration";
        public const string FlareRemoval = "flareRemoval";
        public const string SkySeamFix = "skySeamFix";
        public const string SkySeamStrength = "skySeamStrength";
        public const string SeamEdgeInset = "seamEdgeInset";
        public const string SeamBlend = "seamBlend";
        public const string ParallaxBlend = "parallaxBlend";
        public const string SeamSmoothing = "seamSmoothing";
        public const string NearOffset = "nearOffset";
        public const string FarOffset = "farOffset";
        public const string LensShading = "lensShading";
        public const string ShadingStrength = "shadingStrength";
        public const string ParallaxGrid = "parallaxGrid";
        public const string LensAlignment = "lensAlignment";
        public const string SceneLight = "sceneLight";
        public const string LensFocal = "lensFocal";
        public const string AdvancedGroup = "advancedGroup";
        public const string DlogmCurve = "dlogmCurve";
        public const string Exposure = "exposure";
        public const string RenderDevice = "renderDevice";
        public const string SphereSize = "sphereSize";

        /// <summary>Every name, in OfxSourceParams.h's kAllParams order.</summary>
        public static readonly string[] All =
        {
            ColourGroup, ColorOutput, HdrTone, Look, HdrPeak, Stabilization, StitchGroup,
            SeamSearch, GainMatch, Calibration, FlareRemoval, SkySeamFix, SkySeamStrength,
            SeamEdgeInset, SeamBlend, ParallaxBlend, SeamSmoothing, NearOffset, FarOffset,
            LensShading, ShadingStrength, ParallaxGrid, LensAlignment, SceneLight, LensFocal,
            AdvancedGroup, DlogmCurve, Exposure, RenderDevice, SphereSize,
        };
    }

    /// <summary>The camera controls both effects share (OfxCamera.h).</summary>
    public static class CameraParams
    {
        public const string OutputResolution = "outputResolution";
        public const string CameraGroup = "cameraGroup";
        public const string Preset = "preset";
        public const string Lens = "lens";
        public const string Pan = "pan";
        public const string Tilt = "tilt";
        public const string Roll = "roll";
        public const string DjiFov = "djiFov";
        public const string Correction = "correction";
        public const string Zoom = "zoom";
        public const string Fov = "fov";
        public const string Distortion = "distortion";
        public const string KeyframeEasing = "keyframeEasing";
        public const string SmoothKeyframes = "smoothKeyframes";
        public const string SourceGroup = "sourceGroup";
        public const string SourcePan = "sourcePan";
        public const string SourceTilt = "sourceTilt";
        public const string SourceRoll = "sourceRoll";
        /// <summary>The hidden mirror of the Lens choice (ticked = DJI).</summary>
        public const string LensMirror = "lensMirror";

        /// <summary>Every name, in OfxCamera.h's kAllParams order.</summary>
        public static readonly string[] All =
        {
            OutputResolution, CameraGroup, Preset, Lens, Pan, Tilt, Roll,
            DjiFov, Correction, Zoom, Fov, Distortion, KeyframeEasing,
            SmoothKeyframes, SourceGroup, SourcePan, SourceTilt, SourceRoll, LensMirror,
        };
    }

    /// <summary>
    /// One OpenFX choice parameter's items: the '|'-separated list exactly as
    /// the C++ side declares it, 0-based like every OpenFX choice.
    /// </summary>
    public sealed class ChoiceList
    {
        private readonly string[] _items;

        /// <summary>Wrap an item string such as <c>"Off|Auto"</c>.</summary>
        /// <param name="joined">The items, joined with '|'.</param>
        /// <param name="default0">The 0-based default entry.</param>
        public ChoiceList(string joined, int default0)
        {
            Joined = joined ?? string.Empty;
            _items = Joined.Length == 0 ? new string[0] : Joined.Split('|');
            Default0 = (default0 >= 0 && default0 < _items.Length) ? default0 : 0;
        }

        /// <summary>The items joined with '|', as in the C++ header.</summary>
        public string Joined { get; }

        /// <summary>The 0-based default entry.</summary>
        public int Default0 { get; }

        /// <summary>Number of items.</summary>
        public int Count => _items.Length;

        /// <summary>The item at a 0-based index, or null outside the list.</summary>
        public string this[int index] => (index >= 0 && index < _items.Length) ? _items[index] : null;

        /// <summary>The items.</summary>
        public IReadOnlyList<string> Items => _items;

        /// <summary>The 0-based index of an item (exact, then ignoring case and spaces), or -1.</summary>
        public int IndexOf(string label)
        {
            if (label == null)
            {
                return -1;
            }
            for (int i = 0; i < _items.Length; ++i)
            {
                if (string.Equals(_items[i], label, StringComparison.Ordinal))
                {
                    return i;
                }
            }
            string wanted = Squash(label);
            for (int i = 0; i < _items.Length; ++i)
            {
                if (string.Equals(Squash(_items[i]), wanted, StringComparison.OrdinalIgnoreCase))
                {
                    return i;
                }
            }
            return -1;
        }

        /// <summary>A label with every whitespace character removed.</summary>
        private static string Squash(string s)
        {
            var chars = new List<char>(s.Length);
            foreach (char c in s)
            {
                if (!char.IsWhiteSpace(c))
                {
                    chars.Add(c);
                }
            }
            return new string(chars.ToArray());
        }
    }

    /// <summary>Every choice list the extension reads or writes.</summary>
    public static class Choices
    {
        // ---- the generator's own ------------------------------------------------
        /// <summary>Output (OfxSource.h, kOutputItems).</summary>
        public static readonly ChoiceList Output = new ChoiceList("Reframed view|360 equirect", 0);
        /// <summary>Output: Reframed view.</summary>
        public const int OutputReframed = 0;
        /// <summary>Output: 360 equirect.</summary>
        public const int OutputEquirect = 1;

        /// <summary>Output levels (VEGAS only; default Studio RGB).</summary>
        public static readonly ChoiceList OutputLevels = new ChoiceList("Full range (0-255)|Studio RGB (16-235)", 1);
        /// <summary>Output levels: full range.</summary>
        public const int LevelsFull = 0;
        /// <summary>Output levels: studio RGB.</summary>
        public const int LevelsStudio = 1;

        // ---- colour (SourceSettingsParams.h) ---------------------------------------
        /// <summary>Colour Output; the OpenFX default is Rec. 709 (OfxSourceParams.h, kColorOutputDefault0).</summary>
        public static readonly ChoiceList ColorOutput = new ChoiceList("BT.2100 PQ|BT.2100 HLG|Rec. 709|D-Log M (no transform)", 2);
        public const int ColorPq = 0;
        public const int ColorHlg = 1;
        public const int ColorRec709 = 2;
        public const int ColorDLogM = 3;

        public static readonly ChoiceList HdrTone = new ChoiceList(
            "ACES 2 - Bright (outdoor)|ACES 2 - Detailed (indoor)|BT.2408 - Deep Blacks + Natural|BT.2408 - Deep Blacks + Punchy|BT.2408 - Neutral", 0);
        public static readonly ChoiceList Look = new ChoiceList("DJI (default)|OpenOSV standard", 0);
        public static readonly ChoiceList HdrPeak = new ChoiceList("1000 nits (default)|600 nits|400 nits|203 nits (SDR-safe)", 0);

        /// <summary>Stabilisation; default Smooth + Horizon Lock.</summary>
        public static readonly ChoiceList Stabilization = new ChoiceList("Off|Horizon Lock|Full|Smooth|Smooth + Horizon Lock", 4);
        public const int StabOff = 0;
        public const int StabHorizonLock = 1;
        public const int StabFull = 2;
        public const int StabSmooth = 3;
        public const int StabSmoothLevel = 4;

        // ---- stitching (SourceSettingsParams.h) ------------------------------------
        public static readonly ChoiceList Calibration = new ChoiceList(
            "Auto (as recorded)|Lens Protectors / ND Filters|Underwater|Native (bare lenses)", 0);
        public static readonly ChoiceList SkySeamFix = new ChoiceList("Off|Rim only|Rim and colour", 2);
        public static readonly ChoiceList LensShading = new ChoiceList("Off|Auto", 1);
        public static readonly ChoiceList ParallaxGrid = new ChoiceList("Auto|Steady (per clip)|Follows scene (per moment)", 0);
        public static readonly ChoiceList LensAlignment = new ChoiceList("Auto (fit per clip)|Off (calibration only)", 0);
        /// <summary>Scene Light; default Auto (the camera's metered light, confirmed by the sky).</summary>
        public static readonly ChoiceList SceneLight = new ChoiceList("Auto|Day|Night", 0);
        /// <summary>Lens Focal; default Auto.</summary>
        public static readonly ChoiceList LensFocal = new ChoiceList("Auto|Camera (recorded focal)|Calibration (each lens)", 0);
        public static readonly ChoiceList DlogmCurve = new ChoiceList("DJI Refit|Pocket 3|Osmo 360|Avata 360", 2);
        /// <summary>Render Device, as the Windows build lists it.</summary>
        public static readonly ChoiceList RenderDevice = new ChoiceList("Auto|CPU|CUDA|OpenCL", 0);
        public static readonly ChoiceList SphereSize = new ChoiceList(
            "Native (2 x decoded height)|4K (3840 x 1920)|2560 x 1280|2K (1920 x 960)", 0);

        // ---- the camera (ReframeParams.h, OfxCamera.cpp) ------------------------
        /// <summary>Output Resolution: Premiere's list with "Match Timeline" first (OfxCamera.cpp).</summary>
        public static readonly ChoiceList OutputResolution = new ChoiceList(
            "Match Timeline|3840 x 2160|2560 x 1440|1920 x 1080|1280 x 720", 0);
        /// <summary>Preset; default Wide.</summary>
        public static readonly ChoiceList Preset = new ChoiceList("Custom|Crystal Ball|Asteroid|Wide|Ultra Wide|Dewarping", 3);
        /// <summary>Preset: Custom (writes nothing).</summary>
        public const int PresetCustom = 0;
        /// <summary>Lens; default DJI.</summary>
        public static readonly ChoiceList Lens = new ChoiceList("DJI|Classic", 0);
        public const int LensDji = 0;
        public const int LensClassic = 1;
        /// <summary>Keyframe Easing; default None.</summary>
        public static readonly ChoiceList KeyframeEasing = new ChoiceList(
            "None|Linear Smooth|Fast In, Slow Out|Slow In, Fast Out|Fast In, Fast Out|Slow In, Slow Out|Linear", 0);
    }
}
