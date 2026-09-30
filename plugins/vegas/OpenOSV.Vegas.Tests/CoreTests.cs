// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// CoreTests.cs - OpenOSV.Vegas.Core without VEGAS: JSON, the probe model on
// real osvtool output, time maths, levels, the DJI numbers, proxy timing,
// file rules, settings and the log.

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.Tests
{
    public static class JsonTests
    {
        [Test]
        public static void ParsesEveryKindAndKeepsLongIntegersExact()
        {
            Check.True(Json.TryParse("{\"a\":[1,2.5,-3e2,true,false,null,\"x\\n\\u0041\"],\"ts\":30669420766123456}", out JsonValue v, out string err), "parse: " + err);
            Check.Equal(7, v["a"].Count, "array length");
            Check.Equal(1L, v["a"][0].AsLong(), "int");
            Check.Near(2.5, v["a"][1].AsDouble(), 0, "double");
            Check.Near(-300.0, v["a"][2].AsDouble(), 0, "exponent");
            Check.True(v["a"][3].AsBool(), "true");
            Check.False(v["a"][4].AsBool(true), "false");
            Check.False(v["a"][5].HasValue, "null");
            Check.Equal("x\nA", v["a"][6].AsString(), "escapes");
            // Past 2^53: read from the digits, not through a double.
            Check.Equal(30669420766123456L, v["ts"].AsLong(), "long integer");
        }

        [Test]
        public static void MissingThingsNeverThrow()
        {
            Check.True(Json.TryParse("{\"a\":1}", out JsonValue v, out _), "parse");
            Check.True(v["nope"]["deeper"][3]["still"].IsMissing, "missing chain");
            Check.Equal(42, v["nope"].AsInt(42), "fallback");
            Check.Equal("fb", v["a"].AsString("fb"), "number is not a string");
            Check.Equal(0L, v["a"][0].AsLong(0), "number is not an array");
        }

        [Test]
        public static void RejectsMalformedDocumentsWithoutThrowing()
        {
            foreach (string bad in new[] { "", "{", "{\"a\":}", "[1,]", "{\"a\" 1}", "tru", "\"unterminated", "{\"a\":1,}", "nul" })
            {
                Check.False(Json.TryParse(bad, out _, out string err), "should reject: " + bad);
                Check.NotNull(err, "an error for: " + bad);
            }
            // Deep nesting is refused, not a stack overflow.
            string deep = new string('[', 5000) + new string(']', 5000);
            Check.False(Json.TryParse(deep, out _, out _), "deep nesting");
        }

        [Test]
        public static void FindsTheDocumentInsideConsoleOutput()
        {
            string mixed = "file: x.OSV (12 bytes)\nformat: ...\n{\n  \"frameCount\": 65\n}\nwrote -\n";
            Check.True(Json.TryParseEmbeddedObject(mixed, out JsonValue v, out string err), "embedded: " + err);
            Check.Equal(65L, v["frameCount"].AsLong(), "frameCount");
            Check.False(Json.TryParseEmbeddedObject("no json here { not at a line start", out _, out _), "no document");
        }

        [Test]
        public static void WriterRoundTripsAndEscapes()
        {
            var w = new JsonWriter();
            w.BeginObject().Value("s", "a\"b\\c\nd").Value("n", 0.1).Value("i", 7L).Value("b", true).BeginArray("e").EndArray().EndObject();
            Check.True(Json.TryParse(w.ToString(), out JsonValue v, out string err), "round trip: " + err);
            Check.Equal("a\"b\\c\nd", v["s"].AsString(), "string");
            Check.Near(0.1, v["n"].AsDouble(), 0, "double");
            Check.Equal(7L, v["i"].AsLong(), "int");
            Check.True(v["b"].AsBool(), "bool");
            Check.Equal(0, v["e"].Count, "empty array");
        }
    }

    public static class ProbeTests
    {
        [Test]
        public static void ReadsTheSampleOsvFromRealOsvtoolOutput()
        {
            ProbeResult p = ProbeResult.Parse(TestContext.Fixture("probe_osv.json"), null, out string err);
            Check.NotNull(p, "probe: " + err);
            Check.Equal(65L, p.FrameCount, "frames");
            // timescale 60000, 65 samples over 65065 ticks: exactly 60000/1001.
            Check.Equal(60000L, p.Fps.Num, "fps num");
            Check.Equal(1001L, p.Fps.Den, "fps den");
            Check.Near(65.0 * 1001.0 / 60000.0, p.DurationSeconds, 1e-12, "duration");
            Check.Equal(3000, p.StreamWidth, "stream width");
            Check.Equal(3000, p.StreamHeight, "stream height");
            Check.Equal(6000, p.SphereWidth, "native sphere width");
            Check.Equal(3000, p.SphereHeight, "native sphere height");
            Check.Equal("K6", p.Mode, "mode");
            Check.Equal("DLogM", p.ColorModeName, "colour mode");
            Check.True(p.HasAudio, "audio");
            Check.Equal(48000, p.AudioSampleRate, "sample rate");
            Check.Equal(2, p.AudioChannels, "channels");
            Check.False(p.IsLrf, "not a proxy");
            Check.Equal(30669420766L, p.FirstFrameTimestampUs ?? 0, "first frame timestamp");
            Check.Contains(p.Summary, "65 frames at 59.94 fps", "summary");
        }

        [Test]
        public static void ReadsTheSampleLrfFromRealOsvtoolOutput()
        {
            ProbeResult p = ProbeResult.Parse(TestContext.Fixture("probe_lrf.json"), null, out string err);
            Check.NotNull(p, "probe: " + err);
            Check.Equal(124L, p.FrameCount, "frames");
            Check.Equal(30000L, p.Fps.Num, "fps num");
            Check.Equal(1001L, p.Fps.Den, "fps den");
            Check.True(p.IsLrf, "proxy");
            Check.Equal(2048, p.SphereWidth, "proxy sphere width (2 x 1024)");
            Check.Equal(1024, p.SphereHeight, "proxy sphere height");
            Check.Equal(30668353021L, p.FirstFrameTimestampUs ?? 0, "first frame timestamp");
        }

        /// <summary>
        /// The flat stable subset of schema "openosv.probe/1" (WP-V-CLI's
        /// `probe --json -`), real output trimmed to those keys alone: the
        /// same clip must read exactly as it does from the older layout.
        /// </summary>
        [Test]
        public static void ReadsTheFlatSummaryLayout()
        {
            foreach (string pair in new[] { "osv", "lrf" })
            {
                ProbeResult nested = ProbeResult.Parse(TestContext.Fixture("probe_" + pair + ".json"), null, out _);
                ProbeResult flat = ProbeResult.Parse(TestContext.Fixture("probe_" + pair + "_v1.json"), null, out string err);
                Check.NotNull(flat, pair + " flat: " + err);
                Check.Equal(nested.Fps, flat.Fps, pair + " rational fps");
                Check.Equal(nested.FrameCount, flat.FrameCount, pair + " frames");
                Check.Equal(nested.SphereWidth, flat.SphereWidth, pair + " sphere");
                Check.Equal(nested.Mode, flat.Mode, pair + " mode");
                Check.Equal(nested.ColorModeName, flat.ColorModeName, pair + " colour");
                Check.Equal(nested.IsLrf, flat.IsLrf, pair + " proxy");
                Check.Equal(nested.AudioChannels, flat.AudioChannels, pair + " channels");
                Check.Equal(nested.AudioSampleRate, flat.AudioSampleRate, pair + " sample rate");
                Check.Equal(nested.AudioSampleCount, flat.AudioSampleCount, pair + " audio samples");
                Check.Equal(nested.FirstFrameTimestampUs, flat.FirstFrameTimestampUs, pair + " timestamp");
                Check.Near(nested.DurationSeconds, flat.DurationSeconds, 2e-3, pair + " duration");
            }
            // The sample's audio: 52 224 samples at 48 kHz.
            ProbeResult osv = ProbeResult.Parse(TestContext.Fixture("probe_osv_v1.json"), null, out _);
            Check.Equal(52224L, osv.AudioSampleCount, "sample count");
            Check.Near(1.088, osv.AudioDurationSeconds, 1e-9, "audio seconds");
        }

        /// <summary>osvtool states null for what it cannot tell; the parser falls back, never throws.</summary>
        [Test]
        public static void NullFieldsFallBack()
        {
            string text = TestContext.Fixture("probe_osv_v1.json")
                .Replace("\"mode\": \"K6\"", "\"mode\": null")
                .Replace("\"streamW\": 3000", "\"streamW\": null");
            ProbeResult p = ProbeResult.Parse(text, null, out string err);
            Check.NotNull(p, "still usable: " + err);
            Check.Equal(string.Empty, p.Mode, "unknown mode");
            Check.Equal(0, p.StreamWidth, "unknown width");
            Check.Equal(6000, p.SphereWidth, "the sphere follows the lens height");
        }

        [Test]
        public static void RefusesDocumentsWithoutAClip()
        {
            Check.Null(ProbeResult.Parse("{\"frameCount\":0}", "x.OSV", out string err), "no frames");
            Check.NotNull(err, "error");
            Check.Null(ProbeResult.Parse("error: cannot open", "x.OSV", out err), "not JSON");
            Check.Contains(err, "not JSON", "error text");
        }
    }

    public static class TimeTests
    {
        [Test]
        public static void RationalsReduceAndRecogniseBroadcastRates()
        {
            Check.True(Rational.TryCreate(120000, 2002, out Rational r), "create");
            Check.Equal("60000/1001", r.ToString(), "reduced");
            Check.True(Rational.TryFromDouble(59.94005994005994, out r), "59.94");
            Check.Equal("60000/1001", r.ToString(), "59.94 as rational");
            Check.True(Rational.TryFromDouble(29.97, out r), "29.97");
            Check.Equal("30000/1001", r.ToString(), "29.97 as rational");
            Check.True(Rational.TryFromDouble(25.0, out r), "25");
            Check.Equal("25/1", r.ToString(), "25 as rational");
            Check.True(Rational.TryFromDouble(12.5, out r), "12.5");
            Check.Equal("25/2", r.ToString(), "12.5 as rational");
            Check.False(Rational.TryFromDouble(double.NaN, out _), "NaN");
            Check.False(Rational.TryCreate(0, 1, out _), "zero");
        }

        [Test]
        public static void FramesToUnitsIsExactAndRounded()
        {
            Rational.TryCreate(60000, 1001, out Rational fps);
            // 65 frames = 65 065 / 60 000 s = 1.08441666... s = 10 844 166.67 units of 100 ns.
            Check.Equal(10844167L, TimeMath.FramesToUnits(65, fps, 10_000_000), "65 frames at 59.94");
            // An hour of 59.94: 215 784 frames last 215 999 784 / 60 000 = 3 599.9964 s exactly,
            // not the 3 600 s that 215 784 / 59.94 would give.
            Check.Equal(35999964000L, TimeMath.FramesToUnits(215784, fps, 10_000_000), "one hour");
            Check.Equal(-1L, TimeMath.FramesToUnits(-1, fps, 10_000_000), "negative");
            Check.Equal(0L, TimeMath.FramesToUnits(0, fps, 10_000_000), "zero");
        }

        [Test]
        public static void SecondsToFrameUsesTheGeneratorsTolerance()
        {
            Rational.TryCreate(60000, 1001, out Rational fps);
            // Exactly frame 3's start must be frame 3, not 2.
            Check.Equal(3L, TimeMath.SecondsToFrame(3.0 * 1001.0 / 60000.0, fps), "boundary");
            Check.Equal(0L, TimeMath.SecondsToFrame(0.0, fps), "zero");
        }

        [Test]
        public static void FormatsDurationsAndRates()
        {
            Check.Equal("0:01.08", TimeMath.FormatDuration(1.0845), "short");
            Check.Equal("1:02:03.50", TimeMath.FormatDuration(3723.5), "long");
            Check.Equal("0:00.00", TimeMath.FormatDuration(double.NaN), "nonsense");
            Rational.TryCreate(24000, 1001, out Rational r);
            Check.Equal("23.976", TimeMath.FormatFps(r), "23.976");
        }
    }

    public static class LevelsTests
    {
        [Test]
        public static void PixelFormatNamesDecideLevels()
        {
            Check.Equal(ProjectLevels.Studio, ProjectColour.LevelsFor("Int8Bit"), "8-bit");
            Check.Equal(ProjectLevels.Studio, ProjectColour.LevelsFor("Float32Bit"), "32-bit video levels");
            Check.Equal(ProjectLevels.Full, ProjectColour.LevelsFor("Float32BitFullRange"), "32-bit full range");
            // A newer VEGAS's name the extension was never compiled against.
            Check.Equal(ProjectLevels.Full, ProjectColour.LevelsFor("Int8BitFullRange"), "8-bit full range");
            Check.Equal(ProjectLevels.Studio, ProjectColour.LevelsFor(null), "unknown");
            Check.Equal(Choices.LevelsStudio, ProjectColour.OutputLevelsIndexFor("Int8Bit"), "studio index");
            Check.Equal(Choices.LevelsFull, ProjectColour.OutputLevelsIndexFor("Float32BitFullRange"), "full index");
            Check.Equal("32-bit floating point (full range)", ProjectColour.Describe("Float32BitFullRange"), "describe");
            Check.Equal(ProjectHdr.Hlg, ProjectColour.HdrFor("HLG"), "HLG");
            Check.Equal(ProjectHdr.Hdr10, ProjectColour.HdrFor("HDR10"), "HDR10");
            Check.Equal(ProjectHdr.Off, ProjectColour.HdrFor("Off"), "off");
        }
    }

    public static class CameraTests
    {
        [Test]
        public static void LooksWriteDjiStudiosNumbersForTheFrameShape()
        {
            FramingLook wide = DjiCamera.LookById("wide");
            FramingWrites landscape = DjiCamera.WritesFor(wide, 16.0 / 9.0);
            Check.Near(60.0, landscape.DjiFov, 0, "wide landscape fov");
            Check.Near(142.4, landscape.Zoom, 1e-9, "wide zoom read-out on 16:9");
            Check.Equal(3, landscape.PresetIndex0, "preset entry");
            Check.Equal(Choices.LensDji, landscape.LensIndex0, "DJI lens");
            Check.True(landscape.LensMirror, "mirror ticked");
            Check.Near(90.0, DjiCamera.WritesFor(wide, 9.0 / 16.0).DjiFov, 0, "9:16 column");
            Check.Near(72.0, DjiCamera.WritesFor(wide, 3.0 / 4.0).DjiFov, 0, "3:4 column");
            Check.Near(-90.0, DjiCamera.WritesFor(DjiCamera.LookById("asteroid"), 16.0 / 9.0).Tilt, 0, "asteroid tilt");
            Check.Equal(5, DjiCamera.Looks.Count, "five looks");
        }

        [Test]
        public static void FramingAspectFollowsResolutionThenFrame()
        {
            Check.Near(16.0 / 9.0, DjiCamera.FramingAspect(0, 0, 0), 1e-12, "unknown frame");
            Check.Near(2.0, DjiCamera.FramingAspect(0, 3840, 1920), 1e-12, "frame");
            Check.Near(1280.0 / 720.0, DjiCamera.FramingAspect(4, 3840, 1920), 1e-12, "fixed entry");
        }

        [Test]
        public static void ZoomIsZeroForNonsense()
        {
            Check.Near(0.0, DjiCamera.ZoomDeg(double.NaN, 0.6, 1.7), 0, "NaN");
            Check.Near(0.0, DjiCamera.ZoomDeg(60, -1, 1.7), 0, "negative correction");
            Check.Near(0.0, DjiCamera.ZoomDeg(60, 0.6, 0), 0, "zero aspect");
        }

        [Test]
        public static void StabilisationSwitchesMapOntoThePopup()
        {
            Check.Equal(Choices.StabOff, Stabilisation.IndexFor(false, false), "off");
            Check.Equal(Choices.StabHorizonLock, Stabilisation.IndexFor(false, true), "horizon");
            Check.Equal(Choices.StabSmooth, Stabilisation.IndexFor(true, false), "rocksteady");
            Check.Equal(Choices.StabSmoothLevel, Stabilisation.IndexFor(true, true), "both");
            Check.False(Stabilisation.TrySwitchesFor(Choices.StabFull, out _, out _), "full has no switches");
            for (int i = 0; i < Choices.Stabilization.Count; ++i)
            {
                if (Stabilisation.TrySwitchesFor(i, out bool rs, out bool hl))
                {
                    Check.Equal(i, Stabilisation.IndexFor(rs, hl), "round trip " + i);
                }
            }
        }

        [Test]
        public static void EasingSpeedsAreTheTilesCurves()
        {
            Check.Near(1.5, DjiCamera.EaseSpeed("slow-in-slow-out", 0.5), 1e-12, "slow-slow middle");
            Check.Near(0.0, DjiCamera.EaseSpeed("slow-in-slow-out", 0.0), 1e-12, "slow-slow start");
            Check.Near(2.0, DjiCamera.EaseSpeed("fast-in-slow-out", 0.0), 1e-12, "fast start");
            Check.Near(1.0, DjiCamera.EaseSpeed("linear", 0.3), 1e-12, "linear");
        }
    }

    public static class ProxyTests
    {
        /// <summary>
        /// The sample pair: the .OSV's first frame is 1.067745 s into the .LRF
        /// (camera timestamps 30669420766 and 30668353021 us), so Start Frame 0
        /// on the .OSV is .LRF frame round(1.067745 x 29.97) = 32.
        /// </summary>
        [Test]
        public static void StartFrameFollowsTheCameraClock()
        {
            ProbeResult osv = ProbeResult.Parse(TestContext.Fixture("probe_osv.json"), null, out _);
            ProbeResult lrf = ProbeResult.Parse(TestContext.Fixture("probe_lrf.json"), null, out _);
            Check.Equal(32L, ProxyTiming.MapStartFrame(0, osv, lrf), "OSV 0 -> LRF");
            // OSV frame 30 is half a second later: 15 proxy frames on.
            Check.Equal(47L, ProxyTiming.MapStartFrame(30, osv, lrf), "OSV 30 -> LRF");
            // And back: LRF 32 is OSV frame 0 (within rounding).
            Check.Equal(0L, ProxyTiming.MapStartFrame(32, lrf, osv), "LRF 32 -> OSV");
            Check.True(ProxyTiming.Overlaps(osv, lrf), "the pair overlaps");
        }

        [Test]
        public static void StartFrameClampsIntoTheTarget()
        {
            Rational.TryCreate(30, 1, out Rational fps);
            Check.Equal(9L, ProxyTiming.MapStartFrame(1000, fps, null, fps, null, 10), "past the end");
            Check.Equal(0L, ProxyTiming.MapStartFrame(0, fps, 5_000_000, fps, 9_000_000, 10), "before the start");
            Check.Equal(5L, ProxyTiming.MapStartFrame(5, fps, null, fps, null, 0), "no timestamps, unknown length");
        }
    }

    public static class FileRuleTests
    {
        [Test]
        public static void ClipExtensionsAreExact()
        {
            Check.True(SiblingFiles.IsOsv(@"C:\a\CAM_0001_D.OSV"), "OSV");
            Check.True(SiblingFiles.IsOsv(@"C:\a\cam.osv"), "osv lower case");
            Check.True(SiblingFiles.IsLrf(@"C:\a\CAM_0001_D.LRF"), "LRF");
            Check.False(SiblingFiles.IsSupported(@"C:\a\clip.osv.mp4"), "osv.mp4");
            Check.False(SiblingFiles.IsSupported(@"C:\trip.osv\"), "folder named .osv");
            Check.False(SiblingFiles.IsSupported(null), "null");
        }

        [Test]
        public static void SiblingsAreFoundInEitherDirection()
        {
            string dir = TestContext.NewDir("siblings");
            string osv = Path.Combine(dir, "CAM_0001_D.OSV");
            string lrf = Path.Combine(dir, "CAM_0001_D.LRF");
            string lone = Path.Combine(dir, "CAM_0002_D.OSV");
            File.WriteAllText(osv, "o");
            File.WriteAllText(lrf, "l");
            File.WriteAllText(lone, "o");
            Check.Equal(lrf, SiblingFiles.ProxyOf(osv), "proxy of OSV");
            Check.Equal(osv, SiblingFiles.OriginalOf(lrf), "original of LRF");
            Check.Null(SiblingFiles.ProxyOf(lone), "no proxy");
            Check.Null(SiblingFiles.OriginalOf(osv), "an OSV has no original");
        }

        [Test]
        public static void ImportPlanPairsAndDeduplicates()
        {
            string dir = TestContext.NewDir("plan");
            string a = Path.Combine(dir, "A.OSV"), aLrf = Path.Combine(dir, "A.LRF");
            string b = Path.Combine(dir, "B.OSV"), bLrf = Path.Combine(dir, "B.LRF");
            string c = Path.Combine(dir, "C.LRF");
            foreach (string f in new[] { a, aLrf, b, bLrf, c })
            {
                File.WriteAllText(f, "x");
            }
            List<ImportItem> plan = ImportPlan.Plan(new[] { a, aLrf, bLrf, c, a, Path.Combine(dir, "notes.txt") });
            Check.Equal(4, plan.Count, "four clip files (duplicate and text dropped)");
            Check.Equal(ImportKind.Osv, plan[0].Kind, "A.OSV");
            Check.NotNull(plan[1].SkipReason, "A.LRF skipped: its OSV is in the batch");
            Check.Equal(ImportKind.LrfAsProxy, plan[2].Kind, "B.LRF presented on B.OSV");
            Check.Equal(b, plan[2].TimelinePath, "B's timeline is B.OSV");
            Check.Null(plan[2].SkipReason, "B.LRF imported");
            Check.Equal(ImportKind.LrfAlone, plan[3].Kind, "C.LRF alone");
        }

        [Test]
        public static void FrameSizesPerOutput()
        {
            ImportPlan.FrameSize(Choices.OutputReframed, 1920, 1080, out int w, out int h);
            Check.Equal(1920, w, "reframed w"); Check.Equal(1080, h, "reframed h");
            ImportPlan.FrameSize(Choices.OutputEquirect, 1920, 1080, out w, out h);
            Check.Equal(1920, w, "equirect w in a 16:9 project"); Check.Equal(960, h, "equirect h");
            ImportPlan.FrameSize(Choices.OutputEquirect, 5760, 2880, out w, out h);
            Check.Equal(5760, w, "equirect in a 2:1 project"); Check.Equal(2880, h, "2:1 h");
            ImportPlan.FrameSize(Choices.OutputReframed, 0, 0, out w, out h);
            Check.Equal(1920, w, "no project w"); Check.Equal(1080, h, "no project h");
            ImportPlan.FrameSize(Choices.OutputReframed, 1081, 721, out w, out h);
            Check.Equal(1082, w, "even w"); Check.Equal(722, h, "even h");
            ImportPlan.Project360Size(1920, 1080, out w, out h);
            Check.Equal(3840, w, "360 setup w"); Check.Equal(1920, h, "360 setup h");
            ImportPlan.Project360Size(7680, 3840, out w, out h);
            Check.Equal(7680, w, "already 2:1");
        }

        [Test]
        public static void RelinkFindsByNameAndPrefersTheRightSize()
        {
            string root = TestContext.NewDir("relink");
            string deep = Path.Combine(root, "2026", "trip", "day1");
            string other = Path.Combine(root, "backup");
            Directory.CreateDirectory(deep);
            Directory.CreateDirectory(other);
            File.WriteAllText(Path.Combine(other, "CAM_0001_D.OSV"), "short");
            File.WriteAllText(Path.Combine(deep, "cam_0001_d.osv"), "the real one");
            var found = RelinkSearch.FindByName(root, new[] { "CAM_0001_D.OSV", "MISSING.OSV" }, default(System.Threading.CancellationToken));
            Check.True(found.ContainsKey("CAM_0001_D.OSV"), "found by name, any case");
            Check.Equal(2, found["CAM_0001_D.OSV"].Count, "both copies");
            Check.False(found.ContainsKey("MISSING.OSV"), "missing stays missing");
            string best = RelinkSearch.PickBest(@"X:\old\day1\CAM_0001_D.OSV", found["CAM_0001_D.OSV"], "the real one".Length);
            Check.Contains(best, "day1", "size wins");
            best = RelinkSearch.PickBest(@"X:\old\day1\CAM_0001_D.OSV", found["CAM_0001_D.OSV"], 0);
            Check.Contains(best, "day1", "parent folder name wins without a size");
        }

        [Test]
        public static void CommandLineQuotingSurvivesTheMsvcParser()
        {
            Check.Equal("plain", CommandLine.Quote("plain"), "plain");
            Check.Equal("\"with space\"", CommandLine.Quote("with space"), "space");
            Check.Equal("\"C:\\dir with space\\\\\"", CommandLine.Quote("C:\\dir with space\\"), "trailing backslash doubled");
            Check.Equal("\"say \\\"hi\\\"\"", CommandLine.Quote("say \"hi\""), "quotes escaped");
            Check.Equal("\"\"", CommandLine.Quote(""), "empty");
            Check.Equal("probe \"a b.OSV\" --json -", CommandLine.Join(new[] { "probe", "a b.OSV", "--json", "-" }), "join");
        }
    }

    public static class CacheTests
    {
        [Test]
        public static void CacheKeysFollowPathSizeAndTime()
        {
            var t = new DateTime(2026, 9, 29, 12, 0, 0, DateTimeKind.Utc);
            string k1 = AudioCache.KeyFor(@"C:\A\clip.OSV", 100, t);
            Check.Equal(16, k1.Length, "16 hex digits");
            Check.Equal(k1, AudioCache.KeyFor(@"c:\a\CLIP.osv", 100, t), "path case does not matter");
            Check.False(k1 == AudioCache.KeyFor(@"C:\A\clip.OSV", 101, t), "size matters");
            Check.False(k1 == AudioCache.KeyFor(@"C:\A\clip.OSV", 100, t.AddSeconds(1)), "time matters");
            Check.Equal("CAM_0001_D", AudioCache.AsciiStem("CAM_0001_D"), "ascii kept");
            Check.Equal("Strand_Kiel", AudioCache.AsciiStem("Strand" + (char)0x00E9 + "Kiel").Replace("_", "_"), "non-ascii replaced");
        }

        [Test]
        public static void ValidWavIsReusedAndBrokenOnesAreDropped()
        {
            string dir = TestContext.NewDir("cache");
            string clip = Path.Combine(dir, "CAM_0001_D.OSV");
            File.WriteAllText(clip, "pretend clip bytes");
            var cache = new AudioCache(Path.Combine(dir, "vegas-audio"));
            Check.False(cache.TryGetValid(clip, out string wav, out _), "nothing cached yet");
            Directory.CreateDirectory(cache.Directory);

            // A half-written header is not trusted, and is deleted.
            File.WriteAllBytes(wav, new byte[] { (byte)'R', (byte)'I', (byte)'F', (byte)'F' });
            Check.False(cache.TryGetValid(clip, out wav, out _), "broken file refused");
            Check.False(File.Exists(wav), "broken file deleted");

            // A float WAV of 0.5 s stereo at 48 kHz is.
            WriteFloatWav(wav, 48000, 2, 24000);
            Check.True(cache.TryGetValid(clip, out string again, out WavInfo info), "valid WAV reused");
            Check.Equal(wav, again, "same path");
            Check.True(info.IsFloat32, "float");
            Check.Near(0.5, info.DurationSeconds, 1e-12, "duration");

            // Touching the clip changes the key: the old audio is not reused.
            File.SetLastWriteTimeUtc(clip, DateTime.UtcNow.AddMinutes(-5));
            Check.False(cache.TryGetValid(clip, out _, out _), "stale audio not reused");
        }

        [Test]
        public static void SniffsAacAndWav()
        {
            string dir = TestContext.NewDir("sniff");
            string aac = Path.Combine(dir, "a.wav");
            File.WriteAllBytes(aac, new byte[] { 0xFF, 0xF1, 0x50, 0x80, 0x01, 0x7F, 0xFC });
            Check.Equal(AudioFileKind.AdtsAac, WavFile.Sniff(aac), "ADTS in a .wav name");
            string wav = Path.Combine(dir, "b.wav");
            WriteFloatWav(wav, 44100, 1, 10);
            Check.Equal(AudioFileKind.Wav, WavFile.Sniff(wav), "wav");
            Check.Equal(AudioFileKind.Unknown, WavFile.Sniff(Path.Combine(dir, "none.wav")), "missing");
        }

        /// <summary>A minimal IEEE-float WAV of silence, to exercise the header reader.</summary>
        internal static void WriteFloatWav(string path, int rate, int channels, int frames)
        {
            using (var fs = new FileStream(path, FileMode.Create))
            using (var bw = new BinaryWriter(fs))
            {
                int data = frames * channels * 4;
                bw.Write(Encoding.ASCII.GetBytes("RIFF"));
                bw.Write(36 + data);
                bw.Write(Encoding.ASCII.GetBytes("WAVEfmt "));
                bw.Write(16);
                bw.Write((short)3);
                bw.Write((short)channels);
                bw.Write(rate);
                bw.Write(rate * channels * 4);
                bw.Write((short)(channels * 4));
                bw.Write((short)32);
                bw.Write(Encoding.ASCII.GetBytes("data"));
                bw.Write(data);
                bw.Write(new byte[data]);
            }
        }
    }

    public static class PersistenceTests
    {
        [Test]
        public static void SettingsRoundTripAndSurviveGarbage()
        {
            string dir = TestContext.NewDir("settings");
            string file = Path.Combine(dir, "vegas.json");
            var s = new Settings { OsvToolPath = @"D:\tools\osvtool.exe", DefaultOutput = Choices.OutputEquirect, RockSteady = false, LastLook = "asteroid", HelpOpen = false };
            Check.True(s.Save(file), "save");
            Settings back = Settings.Load(file);
            Check.Equal(s.OsvToolPath, back.OsvToolPath, "path");
            Check.Equal(Choices.OutputEquirect, back.DefaultOutput, "output");
            Check.False(back.RockSteady, "rocksteady");
            Check.Equal("asteroid", back.LastLook, "look");
            Check.False(back.HelpOpen, "help");

            // Garbage in: defaults out, and the bad file kept for a bug report.
            File.WriteAllText(file, "{ this is not json");
            Settings fresh = Settings.Load(file);
            Check.True(fresh.RockSteady, "default rocksteady");
            Check.True(File.Exists(file + ".bad"), "bad file kept");

            // Wrong types and unknown ids fall back per key.
            File.WriteAllText(file, "{\"defaultOutput\":\"banana\",\"lastLook\":\"fisheye\",\"rockSteady\":false}");
            Settings mixed = Settings.Load(file);
            Check.Equal(Choices.OutputReframed, mixed.DefaultOutput, "bad output");
            Check.Equal("wide", mixed.LastLook, "unknown look");
            Check.False(mixed.RockSteady, "good key kept");
        }

        [Test]
        public static void MediaRecordsRoundTrip()
        {
            var r = new MediaRecord { Source = @"E:\DCIM\CAM_0001_D.OSV", Proxy = @"E:\DCIM\CAM_0001_D.LRF", OnProxy = true, FullStartFrame = 30, ProxyStartFrame = 47, Audio = @"C:\cache\a.wav", SourceSize = 123 };
            MediaRecord back = MediaRecord.FromBytes(r.ToBytes());
            Check.NotNull(back, "parsed");
            Check.Equal(r.Source, back.Source, "source");
            Check.True(back.OnProxy, "on proxy");
            Check.Equal(47L, back.ProxyStartFrame, "proxy start");
            Check.Equal(123L, back.SourceSize, "size");
            Check.Null(MediaRecord.FromBytes(new byte[] { 1, 2, 3 }), "garbage");
            Check.Null(MediaRecord.FromBytes(null), "null");
        }

        [Test]
        public static void LogWritesAndRotates()
        {
            string before = Log.FilePath;
            Check.Contains(before, TestContext.Scratch, "the log is redirected away from the user's");
            Log.Info("hello from the tests");
            Check.True(File.Exists(Log.FilePath), "log written");
            Check.Contains(File.ReadAllText(Log.FilePath), "hello from the tests", "line there");
            // Fill past the limit and write once more: it rotates.
            File.AppendAllText(Log.FilePath, new string('x', (int)Log.MaxBytes));
            Log.Warn("after rotation");
            Check.True(File.Exists(Log.FilePath + ".1"), "rotated file");
            Check.Contains(File.ReadAllText(Log.FilePath), "after rotation", "fresh file");
        }

        [Test]
        public static void CopyCountsAndPlurals()
        {
            Check.Equal("1 clip", Copy.Count(1, "clip"), "singular");
            Check.Equal("3 clips", Copy.Count(3, "clip"), "plural");
            Check.Contains(Copy.ImportDone(2, 1, 0, false), "Imported 2 clips. 1 clip didn't make it", "partial import");
            Check.Contains(Copy.ImportDone(0, 2, 0, false), "Nothing imported", "failed import");
            Check.Contains(Copy.ImportDone(1, 0, 0, true), "AAC", "aac note");
        }
    }
}
