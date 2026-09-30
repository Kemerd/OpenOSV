// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProbeResult.cs - what `osvtool probe --json` says about a clip, reduced to
// what the VEGAS extension needs.
//
// ===========================================================================
//  Two document layouts, one model
// ===========================================================================
// osvtool's probe document grew a flat summary for the extension (WP-V-CLI):
//
//   frameCount, fps {num, den, value}, durationSeconds, streamW, streamH,
//   mode, colorModeName, hasAudio, audio {sampleRate, channels}, isLrf
//
// Older osvtool builds - the one a user may still have on PATH - carry the
// same facts deeper: frameCount at the top, format {fps, streamW, streamH,
// mode, colorModeName, sideBySideProxy}, and container.tracks[] with each
// track's timescale / duration / sample count and its audio {sampleRate,
// channels}.  The parser takes the flat field when it is there and derives
// it from the older layout otherwise, so the extension works with both.
//
// The frame rate is always taken as a RATIONAL: from the flat fps object when
// it has num / den, else from the first video track's timescale and sample
// delta (60000 / 1001 for a 59.94 clip), and only as a last resort guessed
// from the double.

using System;
using System.Globalization;
using System.IO;

namespace OpenOSV.Vegas.Core
{
    /// <summary>The facts about one .OSV / .LRF the extension works from.</summary>
    public sealed class ProbeResult
    {
        /// <summary>The path osvtool was given (or reported).</summary>
        public string Path { get; private set; } = string.Empty;

        /// <summary>The file name, for messages.</summary>
        public string FileName => string.IsNullOrEmpty(Path) ? string.Empty : SafeFileName(Path);

        /// <summary>File size in bytes (0 when unknown).</summary>
        public long FileSize { get; private set; }

        /// <summary>Number of video frames on the clip's own timeline.</summary>
        public long FrameCount { get; private set; }

        /// <summary>The clip's frame rate as a rational.</summary>
        public Rational Fps { get; private set; }

        /// <summary>Duration in seconds (FrameCount / Fps unless the document says otherwise).</summary>
        public double DurationSeconds { get; private set; }

        /// <summary>Decoded stream width (one lens for the native files; both side by side for an .LRF).</summary>
        public int StreamWidth { get; private set; }

        /// <summary>Decoded stream height = one lens's height.</summary>
        public int StreamHeight { get; private set; }

        /// <summary>Recording mode name ("K6", "K8", "Lrf", ...).</summary>
        public string Mode { get; private set; } = string.Empty;

        /// <summary>Colour mode name ("DLogM", "Normal", "HLG", ...).</summary>
        public string ColorModeName { get; private set; } = string.Empty;

        /// <summary>Camera model name when the metadata has one.</summary>
        public string CameraModel { get; private set; } = string.Empty;

        /// <summary>True when the file has an audio track.</summary>
        public bool HasAudio { get; private set; }

        /// <summary>Audio sample rate in Hz (0 without audio).</summary>
        public int AudioSampleRate { get; private set; }

        /// <summary>Audio channel count (0 without audio).</summary>
        public int AudioChannels { get; private set; }

        /// <summary>True for an .LRF proxy.</summary>
        public bool IsLrf { get; private set; }

        /// <summary>
        /// The camera's timestamp of the first frame, in microseconds, when the
        /// file carries one.  An .OSV and its .LRF share the clock, which is
        /// how a proxy is lined up with its original (see <see cref="ProxyTiming"/>).
        /// </summary>
        public long? FirstFrameTimestampUs { get; private set; }

        /// <summary>
        /// Width of the clip's native equirect sphere: twice the lens height
        /// (6000 x 3000 for 6K, 2048 x 1024 for the .LRF) - the importer's
        /// "Native (2 x decoded height)".
        /// </summary>
        public int SphereWidth => StreamHeight > 0 ? StreamHeight * 2 : 0;

        /// <summary>Height of the native sphere: the lens height.</summary>
        public int SphereHeight => StreamHeight > 0 ? StreamHeight : 0;

        /// <summary>True when the result can place a clip on a timeline.</summary>
        public bool IsUsable => FrameCount > 0 && Fps.IsValid;

        /// <summary>"K6 - 65 frames at 59.94 fps (0:01.08) - DLogM".</summary>
        public string Summary
        {
            get
            {
                string mode = string.IsNullOrEmpty(Mode) ? "OSV" : Mode;
                string colour = string.IsNullOrEmpty(ColorModeName) ? string.Empty : " - " + ColorModeName;
                return mode + " - " + FrameCount.ToString(CultureInfo.InvariantCulture) + " frames at " +
                       TimeMath.FormatFps(Fps) + " fps (" + TimeMath.FormatDuration(DurationSeconds) + ")" + colour;
            }
        }

        /// <summary>
        /// Parse a probe document.  Returns null with <paramref name="error"/>
        /// set when the text is not a probe document or names no playable clip.
        /// </summary>
        /// <param name="jsonText">The document (stdout or the --json file).</param>
        /// <param name="requestedPath">The path osvtool was asked about, used
        /// when the document's own is missing.</param>
        /// <param name="error">Why the document was refused.</param>
        public static ProbeResult Parse(string jsonText, string requestedPath, out string error)
        {
            error = null;
            if (!Json.TryParseEmbeddedObject(jsonText, out JsonValue doc, out string jsonError))
            {
                error = "osvtool's probe output is not JSON (" + jsonError + ")";
                return null;
            }
            return FromDocument(doc, requestedPath, out error);
        }

        /// <summary>Build the model from a parsed document (see <see cref="Parse"/>).</summary>
        public static ProbeResult FromDocument(JsonValue doc, string requestedPath, out string error)
        {
            error = null;
            if (doc == null || !doc.IsObject)
            {
                error = "the probe document is not an object";
                return null;
            }
            var r = new ProbeResult();
            JsonValue format = doc["format"];
            JsonValue container = doc["container"];

            // ---- identity -------------------------------------------------------------
            string path = doc["path"].AsString();
            r.Path = !string.IsNullOrEmpty(requestedPath) ? requestedPath : (path ?? string.Empty);
            r.FileSize = Math.Max(0L, doc["size"].AsLong(0));

            // ---- frames and rate -------------------------------------------------------------
            r.FrameCount = Math.Max(0L, doc["frameCount"].AsLong(0));
            r.Fps = ReadRate(doc, format, container);
            if (r.FrameCount <= 0)
            {
                // A document without the metadata track still has the video
                // track's sample count.
                JsonValue video = FirstTrack(container, "Video");
                r.FrameCount = Math.Max(0L, video["sampleCount"].AsLong(0));
            }

            // ---- duration: stated, else frames / rate ------------------------------------------
            double stated = doc["durationSeconds"].AsDouble(double.NaN);
            r.DurationSeconds = (!double.IsNaN(stated) && stated > 0.0)
                ? stated
                : TimeMath.FramesToSeconds(r.FrameCount, r.Fps);

            // ---- geometry and names -------------------------------------------------------------
            r.StreamWidth = FirstPositiveInt(doc["streamW"], format["streamW"]);
            r.StreamHeight = FirstPositiveInt(doc["streamH"], format["streamH"]);
            r.Mode = FirstString(doc["mode"], format["mode"]);
            r.ColorModeName = FirstString(doc["colorModeName"], format["colorModeName"]);
            r.CameraModel = FirstString(doc["cameraModel"], format["cameraModel"]);

            // ---- proxy ----------------------------------------------------------------------------
            if (doc["isLrf"].Kind == JsonKind.Bool)
            {
                r.IsLrf = doc["isLrf"].AsBool(false);
            }
            else
            {
                r.IsLrf = format["sideBySideProxy"].AsBool(false) ||
                          string.Equals(r.Mode, "Lrf", StringComparison.OrdinalIgnoreCase) ||
                          SiblingFiles.IsLrf(r.Path);
            }

            // ---- audio: the flat summary, else the container's audio track ----------------------
            JsonValue audio = doc["audio"];
            JsonValue audioTrack = FirstTrack(container, "Audio");
            if (doc["hasAudio"].Kind == JsonKind.Bool)
            {
                r.HasAudio = doc["hasAudio"].AsBool(false);
            }
            else
            {
                r.HasAudio = audioTrack.HasValue;
            }
            if (r.HasAudio)
            {
                JsonValue trackAudio = audioTrack["audio"];
                r.AudioSampleRate = FirstPositiveInt(audio["sampleRate"], trackAudio["sampleRate"]);
                r.AudioChannels = FirstPositiveInt(audio["channels"], trackAudio["channels"]);
            }

            // ---- the first frame's camera timestamp -----------------------------------------------
            long ts = doc["firstFrameTimestampUs"].AsLong(0);
            if (ts <= 0)
            {
                JsonValue frames = doc["frames"];
                foreach (JsonValue f in frames.Items)
                {
                    if (f["index"].AsLong(-1) == 0)
                    {
                        ts = f["timestampUs"].AsLong(0);
                        break;
                    }
                }
            }
            if (ts <= 0)
            {
                ts = doc["clip"]["header"]["clipTimestampUs"].AsLong(0);
            }
            r.FirstFrameTimestampUs = ts > 0 ? ts : (long?)null;

            // ---- is it a clip at all? ------------------------------------------------------------------
            if (!r.IsUsable)
            {
                error = "osvtool found no playable video in " + (string.IsNullOrEmpty(r.FileName) ? "the file" : r.FileName) +
                        " (" + r.FrameCount.ToString(CultureInfo.InvariantCulture) + " frames, rate " + r.Fps + ")";
                return null;
            }
            return r;
        }

        /// <summary>
        /// The frame rate: the flat fps {num, den}, then fpsNum / fpsDen, then
        /// the first video track's timescale / sample delta, then the double.
        /// </summary>
        private static Rational ReadRate(JsonValue doc, JsonValue format, JsonValue container)
        {
            // ---- the flat summary ------------------------------------------------------
            JsonValue fps = doc["fps"];
            if (fps.IsObject)
            {
                long num = FirstLong(fps["num"], fps["numerator"]);
                long den = FirstLong(fps["den"], fps["denominator"]);
                if (Rational.TryCreate(num, den, out Rational flat))
                {
                    return flat;
                }
            }
            if (Rational.TryCreate(doc["fpsNum"].AsLong(0), doc["fpsDen"].AsLong(0), out Rational pair))
            {
                return pair;
            }

            // ---- the container: timescale / (duration / samples) ------------------------
            JsonValue video = FirstTrack(container, "Video");
            long timescale = video["timescale"].AsLong(0);
            long duration = video["duration"].AsLong(0);
            long samples = video["sampleCount"].AsLong(0);
            if (timescale > 0 && duration > 0 && samples > 0 && duration % samples == 0)
            {
                if (Rational.TryCreate(timescale, duration / samples, out Rational fromTrack))
                {
                    return fromTrack;
                }
            }

            // ---- a double, recognised or approximated -----------------------------------
            double value = fps.Kind == JsonKind.Number ? fps.AsDouble(0) : fps["value"].AsDouble(0);
            if (!(value > 0.0))
            {
                value = format["fps"].AsDouble(0);
            }
            if (!(value > 0.0))
            {
                value = video["fps"].AsDouble(0);
            }
            return Rational.TryFromDouble(value, out Rational guessed) ? guessed : default(Rational);
        }

        /// <summary>The first container track of a kind ("Video", "Audio"), or Missing.</summary>
        private static JsonValue FirstTrack(JsonValue container, string kind)
        {
            foreach (JsonValue t in container["tracks"].Items)
            {
                if (string.Equals(t["kind"].AsString(), kind, StringComparison.OrdinalIgnoreCase))
                {
                    return t;
                }
            }
            return JsonValue.Missing;
        }

        private static int FirstPositiveInt(JsonValue a, JsonValue b)
        {
            int v = a.AsInt(0);
            if (v > 0)
            {
                return v;
            }
            // Audio sample rates are doubles in the older layout (48000.0).
            double d = a.AsDouble(0);
            if (d >= 1.0 && d < int.MaxValue)
            {
                return (int)Math.Round(d);
            }
            v = b.AsInt(0);
            if (v > 0)
            {
                return v;
            }
            d = b.AsDouble(0);
            return (d >= 1.0 && d < int.MaxValue) ? (int)Math.Round(d) : 0;
        }

        private static long FirstLong(JsonValue a, JsonValue b)
        {
            long v = a.AsLong(0);
            return v != 0 ? v : b.AsLong(0);
        }

        private static string FirstString(JsonValue a, JsonValue b)
        {
            string s = a.AsString();
            if (!string.IsNullOrEmpty(s))
            {
                return s;
            }
            return b.AsString() ?? string.Empty;
        }

        /// <summary>Path.GetFileName that never throws on odd characters.</summary>
        internal static string SafeFileName(string path)
        {
            try
            {
                return System.IO.Path.GetFileName(path);
            }
            catch (ArgumentException)
            {
                int cut = Math.Max(path.LastIndexOf('\\'), path.LastIndexOf('/'));
                return cut >= 0 ? path.Substring(cut + 1) : path;
            }
        }
    }
}
