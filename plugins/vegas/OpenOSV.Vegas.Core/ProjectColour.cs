// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProjectColour.cs - what a VEGAS project's pixel format and HDR mode mean for
// OpenOSV Source's output, decided by NAME.
//
// ===========================================================================
//  Why names, not numbers
// ===========================================================================
// The VEGAS scripting API's PixelFormat enum is {Int8Bit, Float32Bit,
// Float32BitFullRange} in VEGAS 17, and later releases add members (an 8-bit
// full-range mode among them).  An extension compiled against 17 but running
// in a newer VEGAS would misread a renumbered value, so the extension reads
// the enum's NAME at run time and decides here:
//
//   a name containing "FullRange"  -> full range (0-255)
//   anything else                  -> "video levels" = studio RGB (16-235)
//
// VEGAS never level-converts a generator's output, so OpenOSV Source must be
// told which one to write (its outputLevels control).

using System;

namespace OpenOSV.Vegas.Core
{
    /// <summary>The levels a VEGAS project works in.</summary>
    public enum ProjectLevels
    {
        /// <summary>Studio RGB: 8-bit and 32-bit "video levels" projects.</summary>
        Studio,
        /// <summary>Full range: 32-bit "full range" projects (and newer full-range modes).</summary>
        Full,
    }

    /// <summary>The project's HDR mode, by name.</summary>
    public enum ProjectHdr
    {
        /// <summary>Not an HDR project (or VEGAS too old to have the setting).</summary>
        Off,
        /// <summary>HDR10 (PQ).</summary>
        Hdr10,
        /// <summary>HLG.</summary>
        Hlg,
    }

    /// <summary>Pixel-format and HDR decisions.</summary>
    public static class ProjectColour
    {
        /// <summary>
        /// The levels of a pixel format name.  Null, empty or unknown names are
        /// studio: VEGAS's default 8-bit project works in video levels, so that
        /// is the safer guess when the host cannot say.
        /// </summary>
        public static ProjectLevels LevelsFor(string pixelFormatName)
        {
            if (!string.IsNullOrEmpty(pixelFormatName) &&
                pixelFormatName.IndexOf("FullRange", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return ProjectLevels.Full;
            }
            return ProjectLevels.Studio;
        }

        /// <summary>The 0-based <c>outputLevels</c> entry for a pixel format name.</summary>
        public static int OutputLevelsIndexFor(string pixelFormatName) =>
            LevelsFor(pixelFormatName) == ProjectLevels.Full ? Choices.LevelsFull : Choices.LevelsStudio;

        /// <summary>True for a floating-point pixel format ("Float32Bit...").</summary>
        public static bool IsFloat(string pixelFormatName) =>
            !string.IsNullOrEmpty(pixelFormatName) && pixelFormatName.IndexOf("Float", StringComparison.OrdinalIgnoreCase) >= 0;

        /// <summary>
        /// A pixel format name as VEGAS's Project Properties words it:
        /// "8-bit (video levels)", "32-bit floating point (full range)".
        /// </summary>
        public static string Describe(string pixelFormatName)
        {
            if (string.IsNullOrEmpty(pixelFormatName))
            {
                return "unknown pixel format";
            }
            string depth = IsFloat(pixelFormatName) ? "32-bit floating point" :
                           (pixelFormatName.IndexOf("8", StringComparison.Ordinal) >= 0 ? "8-bit" : pixelFormatName);
            string levels = LevelsFor(pixelFormatName) == ProjectLevels.Full ? "full range" : "video levels";
            return depth + " (" + levels + ")";
        }

        /// <summary>The HDR mode of an HDRMode enum name ("HDR10", "HLG", "Off").</summary>
        public static ProjectHdr HdrFor(string hdrModeName)
        {
            if (string.IsNullOrEmpty(hdrModeName))
            {
                return ProjectHdr.Off;
            }
            if (hdrModeName.IndexOf("HLG", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return ProjectHdr.Hlg;
            }
            if (hdrModeName.IndexOf("HDR10", StringComparison.OrdinalIgnoreCase) >= 0 ||
                hdrModeName.IndexOf("PQ", StringComparison.OrdinalIgnoreCase) >= 0)
            {
                return ProjectHdr.Hdr10;
            }
            return ProjectHdr.Off;
        }

        /// <summary>The label of a levels value, as the outputLevels popup words it.</summary>
        public static string LevelsLabel(ProjectLevels levels) =>
            Choices.OutputLevels[levels == ProjectLevels.Full ? Choices.LevelsFull : Choices.LevelsStudio];
    }
}
