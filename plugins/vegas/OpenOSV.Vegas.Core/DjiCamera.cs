// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// DjiCamera.cs - DJI Studio's framing looks and keyframe easing presets, as
// the numbers the extension writes into the camera controls.
//
// ===========================================================================
//  Why the extension writes every number itself
// ===========================================================================
// When a user picks a Preset in the effect's own controls, the plug-in's
// instance-changed handler (OfxCamera.cpp, instanceChanged) writes the look:
// both lenses' numbers, Tilt, the Zoom read-out, Lens = DJI.  It does that
// only for a change the host reports as a USER edit - and nothing documents
// how VEGAS reports a parameter set from a script.  So, like the Premiere
// panel (panel/shared/osvcore.js, planFramingWrites), the extension writes
// every value the preset would, and writes the Preset popup LAST: if VEGAS
// does report script edits as user edits, the plug-in's own handler then
// rewrites the identical numbers, and the end state is the same either way.
//
// Every number comes from plugins/reframe/ReframeParams.h (kPresetTable) and
// ReframeCpu.cpp (djiZoomDeg, djiPresetFovDeg, framingAspect); the test
// HeaderContractTests reads kPresetTable from the header and compares.

using System;
using System.Collections.Generic;

namespace OpenOSV.Vegas.Core
{
    /// <summary>One of DJI Studio's framing looks (a Preset popup entry that writes a look).</summary>
    public sealed class FramingLook
    {
        internal FramingLook(string id, int presetIndex0, string label, double classicFov, double distortion, double tilt,
                             double djiLandscape, double djiPortrait916, double djiPortrait34, double correction)
        {
            Id = id;
            PresetIndex0 = presetIndex0;
            Label = label;
            ClassicFov = classicFov;
            Distortion = distortion;
            Tilt = tilt;
            DjiFovLandscape = djiLandscape;
            DjiFovPortrait916 = djiPortrait916;
            DjiFovPortrait34 = djiPortrait34;
            Correction = correction;
        }

        /// <summary>A stable id for settings ("crystal-ball").</summary>
        public string Id { get; }

        /// <summary>The 0-based Preset entry (OpenFX numbering).</summary>
        public int PresetIndex0 { get; }

        /// <summary>The button label (DJI Studio's words; "Dewarp" for Dewarping).</summary>
        public string Label { get; }

        /// <summary>Classic lens FOV (degrees).</summary>
        public double ClassicFov { get; }

        /// <summary>Classic lens distortion (percent).</summary>
        public double Distortion { get; }

        /// <summary>Tilt the look sets (degrees).</summary>
        public double Tilt { get; }

        /// <summary>DJI FOV on a landscape or square frame.</summary>
        public double DjiFovLandscape { get; }

        /// <summary>DJI FOV on a 9:16 frame.</summary>
        public double DjiFovPortrait916 { get; }

        /// <summary>DJI FOV on a 3:4 frame.</summary>
        public double DjiFovPortrait34 { get; }

        /// <summary>DJI Correction Angle.</summary>
        public double Correction { get; }
    }

    /// <summary>Every value one framing look writes, for one frame shape.</summary>
    public sealed class FramingWrites
    {
        /// <summary>Classic FOV.</summary>
        public double Fov { get; internal set; }
        /// <summary>Classic Distortion.</summary>
        public double Distortion { get; internal set; }
        /// <summary>Tilt.</summary>
        public double Tilt { get; internal set; }
        /// <summary>DJI FOV for the frame's shape.</summary>
        public double DjiFov { get; internal set; }
        /// <summary>DJI Correction Angle.</summary>
        public double Correction { get; internal set; }
        /// <summary>The Zoom read-out, rounded to the control's tenths.</summary>
        public double Zoom { get; internal set; }
        /// <summary>Lens entry (DJI).</summary>
        public int LensIndex0 { get; internal set; }
        /// <summary>Preset entry.</summary>
        public int PresetIndex0 { get; internal set; }
        /// <summary>The hidden lens mirror (ticked = DJI).</summary>
        public bool LensMirror { get; internal set; }
    }

    /// <summary>One of DJI Studio's seven keyframe easing presets.</summary>
    public sealed class EasingPreset
    {
        internal EasingPreset(string id, int index0, string label, string line1, string line2)
        {
            Id = id;
            Index0 = index0;
            Label = label;
            Line1 = line1;
            Line2 = line2;
        }

        /// <summary>A stable id ("slow-in-slow-out").</summary>
        public string Id { get; }

        /// <summary>The 0-based Keyframe Easing entry.</summary>
        public int Index0 { get; }

        /// <summary>The popup's item text.</summary>
        public string Label { get; }

        /// <summary>First line of the tile caption.</summary>
        public string Line1 { get; }

        /// <summary>Second line of the tile caption (may be empty).</summary>
        public string Line2 { get; }
    }

    /// <summary>DJI Studio's camera numbers.</summary>
    public static class DjiCamera
    {
        /// <summary>
        /// The five looks, in the Premiere panel's order (Crystal Ball,
        /// Asteroid, Ultra Wide, Wide, Dewarp).  Numbers: ReframeParams.h,
        /// kPresetTable.
        /// </summary>
        public static readonly IReadOnlyList<FramingLook> Looks = new[]
        {
            new FramingLook("crystal-ball", 1, "Crystal Ball", 240.0, 100.0, 0.0, 75.0, 110.0, 87.0, 1.8),
            new FramingLook("asteroid", 2, "Asteroid", 300.0, 100.0, -90.0, 138.0, 147.0, 147.0, 1.0),
            new FramingLook("ultra-wide", 4, "Ultra Wide", 150.0, 40.0, 0.0, 78.0, 110.0, 95.0, 0.5),
            new FramingLook("wide", 3, "Wide", 120.0, 15.0, 0.0, 60.0, 90.0, 72.0, 0.6),
            new FramingLook("dewarping", 5, "Dewarp", 95.0, 0.0, 0.0, 80.0, 112.0, 97.0, 0.2),
        };

        /// <summary>The seven easing presets in DJI Studio's grid order (ReframeParams.h, OSV_REFRAME_EASING_ITEMS).</summary>
        public static readonly IReadOnlyList<EasingPreset> Easings = new[]
        {
            new EasingPreset("none", 0, "None", "None", ""),
            new EasingPreset("linear-smooth", 1, "Linear Smooth", "Linear", "Smooth"),
            new EasingPreset("fast-in-slow-out", 2, "Fast In, Slow Out", "Fast In", "Slow Out"),
            new EasingPreset("slow-in-fast-out", 3, "Slow In, Fast Out", "Slow In", "Fast Out"),
            new EasingPreset("fast-in-fast-out", 4, "Fast In, Fast Out", "Fast In", "Fast Out"),
            new EasingPreset("slow-in-slow-out", 5, "Slow In, Slow Out", "Slow In", "Slow Out"),
            new EasingPreset("linear", 6, "Linear", "Linear", ""),
        };

        /// <summary>The fixed sizes of the Output Resolution entries 1..4 (entry 0 follows the frame).</summary>
        private static readonly int[,] ResolutionSizes = { { 3840, 2160 }, { 2560, 1440 }, { 1920, 1080 }, { 1280, 720 } };

        /// <summary>A look by id, or null.</summary>
        public static FramingLook LookById(string id)
        {
            foreach (FramingLook look in Looks)
            {
                if (string.Equals(look.Id, id, StringComparison.OrdinalIgnoreCase))
                {
                    return look;
                }
            }
            return null;
        }

        /// <summary>An easing preset by id, or null.</summary>
        public static EasingPreset EasingById(string id)
        {
            foreach (EasingPreset e in Easings)
            {
                if (string.Equals(e.Id, id, StringComparison.OrdinalIgnoreCase))
                {
                    return e;
                }
            }
            return null;
        }

        /// <summary>
        /// The shape (width / height) the camera frames for: the Output
        /// Resolution entry's fixed size, else the frame's, else DJI Studio's
        /// 16:9 (ReframeCpu.cpp, framingAspect).
        /// </summary>
        /// <param name="resolutionIndex0">The 0-based Output Resolution entry.</param>
        /// <param name="frameWidth">The frame the effect renders (0 when unknown).</param>
        /// <param name="frameHeight">Its height.</param>
        public static double FramingAspect(int resolutionIndex0, int frameWidth, int frameHeight)
        {
            if (resolutionIndex0 >= 1 && resolutionIndex0 <= ResolutionSizes.GetLength(0))
            {
                return (double)ResolutionSizes[resolutionIndex0 - 1, 0] / ResolutionSizes[resolutionIndex0 - 1, 1];
            }
            if (frameWidth > 0 && frameHeight > 0)
            {
                return (double)frameWidth / frameHeight;
            }
            return 16.0 / 9.0;
        }

        /// <summary>
        /// A look's DJI FOV for a frame shape: the landscape column for a
        /// landscape or square frame; for a portrait one the nearer of 9:16 and
        /// 3:4, split at their geometric mean sqrt(0.5625 x 0.75).
        /// </summary>
        public static double PresetDjiFov(FramingLook look, double aspect)
        {
            if (look == null)
            {
                return double.NaN;
            }
            double a = (double.IsNaN(aspect) || double.IsInfinity(aspect)) ? 16.0 / 9.0 : aspect;
            if (!(a > 0.0) || a >= 1.0)
            {
                return look.DjiFovLandscape;
            }
            return a <= 0.649519052838329 ? look.DjiFovPortrait916 : look.DjiFovPortrait34;
        }

        /// <summary>
        /// DJI Studio's Zoom read-out: the visible horizontal angle of a DJI
        /// lens (vertical pinhole FOV + eye distance behind the centre) on a
        /// frame of shape <paramref name="aspect"/>.  0 for anything DJI would
        /// answer 0 for (osv::geom::djiZoomDeg).
        /// </summary>
        public static double ZoomDeg(double fovDeg, double correction, double aspect)
        {
            if (double.IsNaN(fovDeg) || double.IsNaN(correction) || double.IsNaN(aspect) ||
                double.IsInfinity(fovDeg) || double.IsInfinity(correction) || double.IsInfinity(aspect) ||
                !(aspect > 0.0) || !(fovDeg > 0.0) || correction < 0.0)
            {
                return 0.0;
            }
            double halfV = 0.5 * Math.Min(fovDeg, 180.0) * Math.PI / 180.0;
            double a = Math.Tan(halfV) * aspect;
            if (!(Math.Abs(a) >= 2.220446049250313e-16))
            {
                return 0.0;
            }
            double complement = Math.Atan(1.0 / a);
            double s = Math.Sqrt(1.0 + 1.0 / (a * a));
            double q = Math.Max(-1.0, Math.Min(1.0, correction / s));
            double zoom = 360.0 - 2.0 * complement * 180.0 / Math.PI - 2.0 * Math.Acos(q) * 180.0 / Math.PI;
            return (double.IsNaN(zoom) || double.IsInfinity(zoom)) ? 0.0 : zoom;
        }

        /// <summary>Everything a look writes on a frame of shape <paramref name="aspect"/>.</summary>
        public static FramingWrites WritesFor(FramingLook look, double aspect)
        {
            if (look == null)
            {
                return null;
            }
            double djiFov = PresetDjiFov(look, aspect);
            return new FramingWrites
            {
                Fov = look.ClassicFov,
                Distortion = look.Distortion,
                Tilt = look.Tilt,
                DjiFov = djiFov,
                Correction = look.Correction,
                Zoom = Math.Round(ZoomDeg(djiFov, look.Correction, aspect) * 10.0, MidpointRounding.AwayFromZero) / 10.0,
                LensIndex0 = Choices.LensDji,
                PresetIndex0 = look.PresetIndex0,
                LensMirror = true,
            };
        }

        /// <summary>
        /// Speed along the move at <paramref name="u"/> (0..1 between two
        /// keyframes) for an easing preset, 0..2 around a constant 1 - what the
        /// easing tiles draw (panel/shared/osvcore.js, easeSpeed).
        /// </summary>
        public static double EaseSpeed(string id, double u)
        {
            double x = double.IsNaN(u) ? 0.0 : Math.Max(0.0, Math.Min(1.0, u));
            double smooth = x * x * (3.0 - 2.0 * x);
            switch (id)
            {
                case "slow-in-slow-out": return 6.0 * x * (1.0 - x);
                case "fast-in-fast-out": return 2.0 - 6.0 * x * (1.0 - x);
                case "fast-in-slow-out": return 2.0 * (1.0 - smooth);
                case "slow-in-fast-out": return 2.0 * smooth;
                // Linear Smooth's speed comes from its neighbours; the tile
                // draws it as a gentle wave through the straight line's level.
                case "linear-smooth": return 1.0 + 0.35 * Math.Sin(2.0 * Math.PI * x);
                default: return 1.0;
            }
        }
    }
}
