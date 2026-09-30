// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImportPlan.cs - the decisions of an import that need no VEGAS: which files
// become which clips, how big each generated frame is, and what the extension
// remembers on each media.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;

namespace OpenOSV.Vegas.Core
{
    /// <summary>How one chosen file becomes a clip.</summary>
    public enum ImportKind
    {
        /// <summary>An .OSV, on its own timeline.</summary>
        Osv,
        /// <summary>
        /// An .LRF whose .OSV is beside it: presented on the .OSV's timeline
        /// (length, audio) and played from the .LRF - "Edit with LRF proxies"
        /// already switched on for this clip.
        /// </summary>
        LrfAsProxy,
        /// <summary>An .LRF with no .OSV beside it, on its own timeline.</summary>
        LrfAlone,
    }

    /// <summary>One file of an import batch.</summary>
    public sealed class ImportItem
    {
        /// <summary>The file the user chose.</summary>
        public string ChosenPath { get; internal set; }

        /// <summary>How it is imported.</summary>
        public ImportKind Kind { get; internal set; }

        /// <summary>The .OSV whose timeline the clip uses (for an .OSV, itself).</summary>
        public string TimelinePath { get; internal set; }

        /// <summary>Why it is skipped, or null when it is imported.</summary>
        public string SkipReason { get; internal set; }

        /// <summary>File name for messages.</summary>
        public string Name => ProbeResult.SafeFileName(ChosenPath ?? string.Empty);
    }

    /// <summary>The import rules.</summary>
    public static class ImportPlan
    {
        /// <summary>
        /// Turn chosen files into import items, in the order chosen.  Files
        /// that are not clips are dropped.  When a batch holds both halves of a
        /// pair (X.OSV and X.LRF), the .OSV is imported and the .LRF skipped -
        /// one recording, one clip.  An .LRF whose .OSV is beside it (but not
        /// in the batch) becomes a proxy-presented clip of that .OSV.
        /// </summary>
        public static List<ImportItem> Plan(IEnumerable<string> chosen)
        {
            List<string> clips = SiblingFiles.FilterClips(chosen);
            var osvInBatch = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            foreach (string c in clips)
            {
                if (SiblingFiles.IsOsv(c))
                {
                    osvInBatch.Add(c);
                }
            }

            var items = new List<ImportItem>();
            foreach (string c in clips)
            {
                var item = new ImportItem { ChosenPath = c };
                if (SiblingFiles.IsOsv(c))
                {
                    item.Kind = ImportKind.Osv;
                    item.TimelinePath = c;
                }
                else
                {
                    string original = SiblingFiles.OriginalOf(c);
                    if (original != null && osvInBatch.Contains(SiblingFiles.NormalizeOrSelf(original)))
                    {
                        item.Kind = ImportKind.LrfAsProxy;
                        item.TimelinePath = original;
                        item.SkipReason = "its .OSV is in this import";
                    }
                    else if (original != null)
                    {
                        item.Kind = ImportKind.LrfAsProxy;
                        item.TimelinePath = original;
                    }
                    else
                    {
                        item.Kind = ImportKind.LrfAlone;
                        item.TimelinePath = c;
                    }
                }
                items.Add(item);
            }
            return items;
        }

        /// <summary>
        /// The generated frame size for an output mode.
        /// <list type="bullet">
        /// <item>Reframed view: the project's frame, so the view is rendered
        /// once at the size it is shown.</item>
        /// <item>360 equirect: the project's frame when the project is already
        /// 2:1 (a 360 project), else the project's width by half of it - the
        /// whole sphere at the width the project delivers, which VEGAS
        /// letterboxes into a non-360 frame.</item>
        /// </list>
        /// Sizes are even (video encoders' 4:2:0 wants that) and between 16 and
        /// 8192 per edge; a project that cannot be read gives 1920 x 1080 /
        /// 3840 x 1920.
        /// </summary>
        public static void FrameSize(int outputIndex0, int projectWidth, int projectHeight, out int width, out int height)
        {
            bool haveProject = projectWidth >= 16 && projectHeight >= 16 && projectWidth <= 16384 && projectHeight <= 16384;
            if (outputIndex0 == Choices.OutputEquirect)
            {
                if (haveProject && IsTwoToOne(projectWidth, projectHeight))
                {
                    width = Even(projectWidth);
                    height = Even(projectHeight);
                }
                else
                {
                    int w = haveProject ? projectWidth : 3840;
                    width = Even(Clamp(w, 32, 8192));
                    height = Even(Clamp(width / 2, 16, 4096));
                }
                return;
            }
            width = Even(Clamp(haveProject ? projectWidth : 1920, 16, 8192));
            height = Even(Clamp(haveProject ? projectHeight : 1080, 16, 8192));
        }

        /// <summary>True when a frame is 2:1 within half a percent.</summary>
        public static bool IsTwoToOne(int width, int height) =>
            width > 0 && height > 0 && Math.Abs((double)width / height - 2.0) < 0.01;

        /// <summary>
        /// The 2:1 project size "360 project setup" picks: the project's own
        /// size when it is already 2:1, else 3840 x 1920 (YouTube's and most
        /// players' 4K 360) - or the project's width by half when it is wider.
        /// </summary>
        public static void Project360Size(int projectWidth, int projectHeight, out int width, out int height)
        {
            if (IsTwoToOne(projectWidth, projectHeight) && projectWidth >= 16 && projectWidth <= 8192)
            {
                width = Even(projectWidth);
                height = Even(projectHeight);
                return;
            }
            width = Even(Clamp(Math.Max(3840, projectWidth), 32, 8192));
            height = width / 2;
        }

        private static int Even(int v) => (v & 1) == 0 ? v : v + 1;

        private static int Clamp(int v, int lo, int hi) => v < lo ? lo : (v > hi ? hi : v);
    }

    /// <summary>
    /// What the extension remembers on each OpenOSV media, stored as JSON in
    /// the media's custom data (so it travels with the project file).
    /// </summary>
    /// <remarks>
    /// Nothing depends on this record being there: a media without it (made
    /// by hand from the generator, or by an older extension) is still found
    /// by its generator, and every value is re-derived when missing.  It is
    /// what makes a proxy round trip exact and what lets Relink find the audio
    /// that belongs to a clip.
    /// </remarks>
    public sealed class MediaRecord
    {
        /// <summary>The custom-data key (a fixed GUID, OpenOSV's own).</summary>
        public static readonly Guid DataId = new Guid("6f0b3c1e-5a57-4b8e-9a3d-0e6c1f2a7d41");

        /// <summary>Record format version.</summary>
        public const int CurrentVersion = 1;

        /// <summary>The full-quality clip (.OSV, or a lone .LRF).</summary>
        public string Source { get; set; } = string.Empty;

        /// <summary>The .LRF proxy of <see cref="Source"/> (empty when none is known).</summary>
        public string Proxy { get; set; } = string.Empty;

        /// <summary>True while the generator plays the proxy.</summary>
        public bool OnProxy { get; set; }

        /// <summary>The Start Frame on the full-quality clip.</summary>
        public long FullStartFrame { get; set; }

        /// <summary>The Start Frame the extension wrote when it switched to the proxy (-1 = none).</summary>
        public long ProxyStartFrame { get; set; } = -1;

        /// <summary>The audio file placed with the clip (empty when none).</summary>
        public string Audio { get; set; } = string.Empty;

        /// <summary>Size of <see cref="Source"/> in bytes when imported (for Relink's best match).</summary>
        public long SourceSize { get; set; }

        /// <summary>The record as UTF-8 JSON bytes.</summary>
        public byte[] ToBytes()
        {
            var w = new JsonWriter();
            w.BeginObject();
            w.Value("v", CurrentVersion);
            w.Value("source", Source ?? string.Empty);
            w.Value("proxy", Proxy ?? string.Empty);
            w.Value("onProxy", OnProxy);
            w.Value("fullStartFrame", FullStartFrame);
            w.Value("proxyStartFrame", ProxyStartFrame);
            w.Value("audio", Audio ?? string.Empty);
            w.Value("sourceSize", SourceSize);
            w.EndObject();
            return new System.Text.UTF8Encoding(false).GetBytes(w.ToString());
        }

        /// <summary>A record from custom-data bytes, or null for anything unreadable.</summary>
        public static MediaRecord FromBytes(byte[] bytes)
        {
            if (bytes == null || bytes.Length == 0 || bytes.Length > 1024 * 1024)
            {
                return null;
            }
            string text;
            try
            {
                text = new System.Text.UTF8Encoding(false, true).GetString(bytes);
            }
            catch (Exception)
            {
                return null;
            }
            if (!Json.TryParse(text, out JsonValue doc, out _) || !doc.IsObject)
            {
                return null;
            }
            var r = new MediaRecord
            {
                Source = doc["source"].AsString(string.Empty) ?? string.Empty,
                Proxy = doc["proxy"].AsString(string.Empty) ?? string.Empty,
                OnProxy = doc["onProxy"].AsBool(false),
                FullStartFrame = Math.Max(0L, doc["fullStartFrame"].AsLong(0)),
                ProxyStartFrame = doc["proxyStartFrame"].AsLong(-1),
                Audio = doc["audio"].AsString(string.Empty) ?? string.Empty,
                SourceSize = Math.Max(0L, doc["sourceSize"].AsLong(0)),
            };
            return r;
        }

        /// <summary>For the log.</summary>
        public override string ToString() =>
            "source='" + Source + "' proxy='" + Proxy + "' onProxy=" + OnProxy.ToString(CultureInfo.InvariantCulture) +
            " fullStart=" + FullStartFrame.ToString(CultureInfo.InvariantCulture) +
            " proxyStart=" + ProxyStartFrame.ToString(CultureInfo.InvariantCulture);
    }
}
