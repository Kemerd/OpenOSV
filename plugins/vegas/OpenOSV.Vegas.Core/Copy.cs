// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Copy.cs - every sentence the extension says to a user, in one place.
//
// The voice: confident, short, a little dry.  Say what happened and what to
// do next; never apologise for three lines; never leave the user guessing
// which file failed.

using System;
using System.Collections.Generic;
using System.Globalization;

namespace OpenOSV.Vegas.Core
{
    /// <summary>User-facing text.</summary>
    public static class Copy
    {
        /// <summary>"1 clip" / "3 clips".</summary>
        public static string Count(int n, string singular, string plural = null)
        {
            string word = n == 1 ? singular : (plural ?? singular + "s");
            return n.ToString(CultureInfo.InvariantCulture) + " " + word;
        }

        // ---- product ----------------------------------------------------------------
        public const string ProductName = "OpenOSV";
        public const string Tagline = "360 without the homework.";

        // ---- missing pieces ---------------------------------------------------------------
        public const string NoGenerator =
            "VEGAS can't find OpenOSV Source. Install the OpenOSV OpenFX bundle, then restart VEGAS - it only looks for plug-ins at launch.";
        public const string NoReframeFilter =
            "VEGAS can't find OpenOSV 360 Reframe. Install the OpenOSV OpenFX bundle, then restart VEGAS.";
        public const string NoProject = "Open or create a project first. Even 360 needs somewhere to live.";

        /// <summary>osvtool was not found anywhere.</summary>
        public static string NoOsvTool(IEnumerable<string> searched)
        {
            string places = searched == null ? string.Empty : string.Join("\n  ", searched);
            return "osvtool.exe isn't where it should be. Install the OpenOSV OpenFX bundle, or point the panel at osvtool.exe" +
                   (places.Length > 0 ? ".\n\nLooked in:\n  " + places : ".");
        }

        // ---- selection -------------------------------------------------------------------------
        public const string SelectEventsFirst = "Select some events first. The timeline's not a mind reader.";
        public const string NoOsvInSelection = "None of the selected events is an OpenOSV clip.";
        public const string NoOsvMedia = "No OpenOSV clips in this project yet. Import one and come back.";

        // ---- import -------------------------------------------------------------------------------
        public const string ImportTitle = "Import OSV";
        public const string ImportFilter = "DJI Osmo 360 clips (*.osv;*.lrf)|*.osv;*.lrf|All files (*.*)|*.*";
        public const string NothingToImport = "Nothing to import: pick .OSV or .LRF files.";

        /// <summary>The line after an import.</summary>
        public static string ImportDone(int imported, int failed, int skipped, bool audioFallback)
        {
            string s;
            if (imported == 0 && failed > 0)
            {
                s = "Nothing imported. " + Count(failed, "clip") + " refused to cooperate - details below.";
            }
            else if (failed > 0)
            {
                s = "Imported " + Count(imported, "clip") + ". " + Count(failed, "clip") + " didn't make it - details below.";
            }
            else
            {
                s = "Imported " + Count(imported, "clip") + ". Sized, timed and grouped with audio.";
            }
            if (skipped > 0)
            {
                s += " Skipped " + Count(skipped, "duplicate") + ".";
            }
            if (audioFallback)
            {
                s += " This osvtool only speaks AAC, so audio may lead by ~21 ms. Update the OpenFX bundle for sync-exact WAV.";
            }
            return s;
        }

        // ---- proxies ----------------------------------------------------------------------------------
        /// <summary>After switching clips to proxies or back.</summary>
        public static string ProxyDone(bool toProxy, int switched, int already, int noSibling, int failed)
        {
            string what = toProxy ? "on LRF proxies" : "back on full quality";
            string s = switched > 0 ? Count(switched, "clip") + " " + what + "." : (toProxy ? "No clips switched to proxies." : "No clips switched back.");
            if (already > 0)
            {
                s += " " + Count(already, "clip") + " already were.";
            }
            if (noSibling > 0)
            {
                s += " " + Count(noSibling, "clip") + (toProxy ? " without an .LRF beside the .OSV." : " without the .OSV beside the .LRF.");
            }
            if (failed > 0)
            {
                s += " " + Count(failed, "clip") + " failed - see the log.";
            }
            return s;
        }

        // ---- misc actions ----------------------------------------------------------------------------------
        public static string LookApplied(string look, int targets, bool shared) =>
            look + " on " + Count(targets, "clip") + "." + (shared ? " Events sharing a clip share its camera - use Make framing unique to split them." : string.Empty);

        public static string EasingApplied(string easing, int targets) => "Keyframe easing: " + easing + " on " + Count(targets, "clip") + ".";

        public static string StabilisationApplied(string caption, int targets, int replacedFull) =>
            caption + " on " + Count(targets, "clip") + "." + (replacedFull > 0 ? " Full lock replaced on " + Count(replacedFull, "clip") + "." : string.Empty);

        public static string UniqueDone(int made, int alreadyUnique) =>
            (made > 0 ? Count(made, "event") + " now frame on their own." : "Nothing to split.") +
            (alreadyUnique > 0 ? " " + Count(alreadyUnique, "event") + " already had a camera to themselves." : string.Empty);

        public static string ReframeApplied(int applied, int already, int osv) =>
            (applied > 0 ? "OpenOSV 360 Reframe on " + Count(applied, "event") + ", stretched to fill the frame." : "No events changed.") +
            (already > 0 ? " " + Count(already, "event") + " already had it." : string.Empty) +
            (osv > 0 ? " " + Count(osv, "OSV event") + " skipped: OpenOSV Source frames those itself." : string.Empty);

        public static string ProjectSetupDone(int width, int height, int media) =>
            "360 project: " + width.ToString(CultureInfo.InvariantCulture) + " x " + height.ToString(CultureInfo.InvariantCulture) +
            ", 360 output on" + (media > 0 ? ", " + Count(media, "clip") + " switched to 360 equirect." : ".");

        public static string RelinkDone(int relinked, int stillMissing, int audio) =>
            (relinked > 0 ? "Relinked " + Count(relinked, "clip") + "." : "Found nothing to relink.") +
            (audio > 0 ? " Re-extracted audio for " + Count(audio, "clip") + "." : string.Empty) +
            (stillMissing > 0 ? " " + Count(stillMissing, "clip") + " still missing - try a folder higher up." : string.Empty);

        public const string NothingMissing = "Nothing's missing. Every OSV is where you left it.";

        public const string LevelsMismatch = "Levels don't match this project.";

        /// <summary>The help card's HDR note: an honest open question.</summary>
        public const string HdrNote =
            "HDR and ACES projects: untested. OpenOSV Source outputs Rec. 709 by default; in an HDR10 or HLG project try Colour > BT.2100 PQ or HLG and compare against a known clip. Tell us what you see.";

        /// <summary>A crash that was caught.</summary>
        public static string Unexpected(string action, Exception ex) =>
            action + " hit something unexpected: " + (ex == null ? "unknown error" : ex.Message) +
            "\n\nVEGAS is fine. The details are in " + AppPaths.LogFile + ".";
    }
}
