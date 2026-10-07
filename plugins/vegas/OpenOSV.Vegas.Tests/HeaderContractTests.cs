// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HeaderContractTests.cs - the C# constants against the C++ headers they
// repeat.  A renamed OpenFX parameter, a reordered popup or a changed preset
// number fails here, at build-test time, instead of shipping an extension that
// writes to a parameter nobody reads.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.Tests
{
    /// <summary>Just enough C preprocessor and C++ constant parsing for the checks.</summary>
    internal static class CppHeader
    {
        /// <summary><c>inline constexpr const char* kName = "value";</c> constants.</summary>
        public static Dictionary<string, string> StringConstants(string text)
        {
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            var re = new Regex(@"inline\s+constexpr\s+const\s+char\s*\*\s*(k\w+)\s*=\s*((?:""(?:[^""\\]|\\.)*""\s*)+);");
            foreach (Match m in re.Matches(text))
            {
                result[m.Groups[1].Value] = JoinLiterals(m.Groups[2].Value);
            }
            return result;
        }

        /// <summary><c>inline constexpr int|unsigned kName = N;</c> constants.</summary>
        public static Dictionary<string, long> IntConstants(string text)
        {
            var result = new Dictionary<string, long>(StringComparer.Ordinal);
            var re = new Regex(@"inline\s+constexpr\s+(?:int|unsigned)\s+(k\w+)\s*=\s*(-?\d+)\s*;");
            foreach (Match m in re.Matches(text))
            {
                result[m.Groups[1].Value] = long.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
            }
            return result;
        }

        /// <summary>
        /// Every <c>#define NAME value</c>, line continuations joined; string
        /// values are the concatenated literals, others the trimmed text.  The
        /// <c>#if defined(__APPLE__)</c> branch is skipped (these tests describe
        /// the Windows build) and its <c>#else</c> taken.
        /// </summary>
        public static Dictionary<string, string> Defines(string text)
        {
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            string joined = Regex.Replace(text, @"\\\r?\n", " ");
            bool skipping = false;
            foreach (string raw in joined.Split('\n'))
            {
                string line = raw.Trim();
                if (line.StartsWith("#if defined(__APPLE__)", StringComparison.Ordinal))
                {
                    skipping = true;
                    continue;
                }
                if (skipping && (line.StartsWith("#else", StringComparison.Ordinal) || line.StartsWith("#endif", StringComparison.Ordinal)))
                {
                    skipping = false;
                    continue;
                }
                if (skipping)
                {
                    continue;
                }
                Match m = Regex.Match(line, @"^#define\s+(\w+)\s+(.*)$");
                if (!m.Success)
                {
                    continue;
                }
                string value = StripComment(m.Groups[2].Value).Trim();
                result[m.Groups[1].Value] = value.StartsWith("\"", StringComparison.Ordinal) ? JoinLiterals(value) : value;
            }
            return result;
        }

        /// <summary>The identifiers listed in <c>kAllParams[] = { ... };</c>, in order.</summary>
        public static List<string> AllParamsOrder(string text)
        {
            Match m = Regex.Match(text, @"kAllParams\[\]\s*=\s*\{(.*?)\};", RegexOptions.Singleline);
            var list = new List<string>();
            if (!m.Success)
            {
                return list;
            }
            string body = Regex.Replace(m.Groups[1].Value, @"/\*.*?\*/", " ", RegexOptions.Singleline);
            foreach (Match id in Regex.Matches(body, @"\bk\w+\b"))
            {
                list.Add(id.Value);
            }
            return list;
        }

        private static string StripComment(string s)
        {
            int block = s.IndexOf("/*", StringComparison.Ordinal);
            int line = s.IndexOf("//", StringComparison.Ordinal);
            int cut = -1;
            // Only cut at a comment start that is outside every string literal.
            foreach (int candidate in new[] { block, line }.Where(c => c >= 0).OrderBy(c => c))
            {
                if (s.Take(candidate).Count(ch => ch == '"') % 2 == 0)
                {
                    cut = candidate;
                    break;
                }
            }
            return cut >= 0 ? s.Substring(0, cut) : s;
        }

        /// <summary>Concatenate adjacent C string literals and resolve their escapes.</summary>
        public static string JoinLiterals(string text)
        {
            var sb = new StringBuilder();
            foreach (Match m in Regex.Matches(text, @"""((?:[^""\\]|\\.)*)"""))
            {
                string body = m.Groups[1].Value;
                for (int i = 0; i < body.Length; ++i)
                {
                    if (body[i] == '\\' && i + 1 < body.Length)
                    {
                        ++i;
                        sb.Append(body[i] == 'n' ? '\n' : body[i]);
                    }
                    else
                    {
                        sb.Append(body[i]);
                    }
                }
            }
            return sb.ToString();
        }
    }

    public static class HeaderContractTests
    {
        private static string Read(string relative) => File.ReadAllText(TestContext.SourceFile(relative));

        [Test]
        public static void PluginIdentifiersMatchTheOfxHeaders()
        {
            Dictionary<string, string> source = CppHeader.StringConstants(Read("plugins/ofx/OfxSource.h"));
            Dictionary<string, string> reframe = CppHeader.StringConstants(Read("plugins/ofx/OfxReframe.h"));
            Check.Equal(OfxIds.SourcePluginId, source["kPluginId"], "OfxSource.h kPluginId");
            Check.Equal(OfxIds.ReframePluginId, reframe["kPluginId"], "OfxReframe.h kPluginId");
            Check.Equal("{Svfx:org.openosv.OSVSource}", OfxIds.SourceUniqueId, "VEGAS unique id of the generator");
            Check.Equal("{Svfx:org.openosv.Open360Reframe}", OfxIds.ReframeUniqueId, "VEGAS unique id of the filter");
            // The generator's label is the one VEGAS lists it by.
            Check.Contains(Read("plugins/ofx/OfxSource.cpp"), "\"" + OfxIds.SourceLabel + "\"", "OfxSource.cpp label");
        }

        [Test]
        public static void GeneratorParameterNamesMatchOfxSourceH()
        {
            string text = Read("plugins/ofx/OfxSource.h");
            Dictionary<string, string> s = CppHeader.StringConstants(text);
            Dictionary<string, long> ints = CppHeader.IntConstants(text);
            Check.Equal(SourceParams.File, s["kFile"], "kFile");
            Check.Equal(SourceParams.ChooseFile, s["kChooseFile"], "kChooseFile");
            Check.Equal(SourceParams.ClipInfo, s["kClipInfo"], "kClipInfo");
            Check.Equal(SourceParams.Output, s["kOutput"], "kOutput");
            Check.Equal(SourceParams.StartFrame, s["kStartFrame"], "kStartFrame");
            Check.Equal(Choices.Output.Joined, s["kOutputItems"], "kOutputItems");
            Check.Equal((long)Choices.OutputReframed, ints["kOutputReframed"], "kOutputReframed");
            Check.Equal((long)Choices.OutputEquirect, ints["kOutputEquirect"], "kOutputEquirect");
        }

        /// <summary>
        /// outputLevels is WP-V-OFX's, defined only under a VEGAS host and
        /// therefore NOT in kAllParams: the test finds it by itself.  Every C
        /// string literal of plugins/ofx that holds "Studio RGB (16-235)" must
        /// be exactly the item list the extension writes, and every integer
        /// constant named like an Output Levels default must be its default.
        /// Before WP-V-OFX lands (this branch's base) nothing declares it; the
        /// test then says so and passes, because the extension skips a
        /// parameter the generator does not define.
        /// </summary>
        [Test]
        public static void OutputLevelsMatchesWhereverTheOfxSideDeclaresIt()
        {
            string dir = Path.GetDirectoryName(TestContext.SourceFile("plugins/ofx/OfxSource.h"));
            bool nameFound = false;
            int itemLists = 0;
            foreach (string file in Directory.GetFiles(dir, "*.h").Concat(Directory.GetFiles(dir, "*.cpp")))
            {
                string text = File.ReadAllText(file);
                string shortName = Path.GetFileName(file);

                // ---- the name, as a constant or a literal -------------------------------
                if (text.IndexOf("\"" + SourceParams.OutputLevels + "\"", StringComparison.Ordinal) >= 0)
                {
                    nameFound = true;
                }

                // ---- the items, wherever they are spelled ---------------------------------
                foreach (Match literal in Regex.Matches(text, @"(?:""(?:[^""\\]|\\.)*""\s*)+"))
                {
                    string value = CppHeader.JoinLiterals(literal.Value);
                    if (value.IndexOf("Studio RGB (16-235)", StringComparison.Ordinal) >= 0 && value.IndexOf('|') >= 0)
                    {
                        ++itemLists;
                        Check.Equal(Choices.OutputLevels.Joined, value, shortName + " Output Levels items");
                    }
                }

                // ---- the default, when it is a plain integer constant ------------------------
                foreach (KeyValuePair<string, long> kv in CppHeader.IntConstants(text))
                {
                    if (kv.Key.IndexOf("OutputLevelsDefault", StringComparison.Ordinal) >= 0)
                    {
                        Check.Equal((long)Choices.OutputLevels.Default0, kv.Value, shortName + " " + kv.Key);
                    }
                }
            }
            if (!nameFound)
            {
                Console.WriteLine("        note: no plugins/ofx file declares \"outputLevels\" yet (WP-V-OFX); the extension skips it when absent");
                return;
            }
            Check.True(itemLists > 0, "plugins/ofx names \"outputLevels\" but no item list with \"Studio RGB (16-235)\" was found");
            // Studio is entry 1, Full entry 0: the extension's constants agree.
            Check.Equal("Studio RGB (16-235)", Choices.OutputLevels[Choices.LevelsStudio], "LevelsStudio");
            Check.Equal("Full range (0-255)", Choices.OutputLevels[Choices.LevelsFull], "LevelsFull");
        }

        [Test]
        public static void StitchParameterNamesMatchOfxSourceParamsH()
        {
            string text = Read("plugins/ofx/OfxSourceParams.h");
            Dictionary<string, string> s = CppHeader.StringConstants(text);
            List<string> order = CppHeader.AllParamsOrder(text);
            Check.Equal(StitchParams.All.Length, order.Count, "kAllParams length");
            for (int i = 0; i < order.Count; ++i)
            {
                Check.Equal(StitchParams.All[i], s[order[i]], "OfxSourceParams.h kAllParams[" + i + "] (" + order[i] + ")");
            }
            // The one OpenFX default that differs from Premiere's.
            Check.Equal((long)Choices.ColorOutput.Default0, CppHeader.IntConstants(text)["kColorOutputDefault0"], "kColorOutputDefault0");
        }

        [Test]
        public static void CameraParameterNamesMatchOfxCameraH()
        {
            string text = Read("plugins/ofx/OfxCamera.h");
            Dictionary<string, string> s = CppHeader.StringConstants(text);
            List<string> order = CppHeader.AllParamsOrder(text);
            Check.Equal(CameraParams.All.Length, order.Count, "kAllParams length");
            for (int i = 0; i < order.Count; ++i)
            {
                Check.Equal(CameraParams.All[i], s[order[i]], "OfxCamera.h kAllParams[" + i + "] (" + order[i] + ")");
            }
        }

        [Test]
        public static void SourceSettingsChoiceItemsAndDefaultsMatch()
        {
            Dictionary<string, string> d = CppHeader.Defines(Read("plugins/sourcesettings/SourceSettingsParams.h"));
            var lists = new Dictionary<string, ChoiceList>
            {
                { "OSV_SS_COLOR", Choices.ColorOutput },
                { "OSV_SS_HDR_TONE", Choices.HdrTone },
                { "OSV_SS_LOOK", Choices.Look },
                { "OSV_SS_HDR_PEAK", Choices.HdrPeak },
                { "OSV_SS_STAB", Choices.Stabilization },
                { "OSV_SS_CALIB", Choices.Calibration },
                { "OSV_SS_PHOTO_SEAM", Choices.SkySeamFix },
                { "OSV_SS_LENS_SHADING", Choices.LensShading },
                { "OSV_SS_PARALLAX_GRID", Choices.ParallaxGrid },
                { "OSV_SS_LENS_ALIGN", Choices.LensAlignment },
                { "OSV_SS_SCENE_LIGHT", Choices.SceneLight },
                { "OSV_SS_LENS_FOCAL", Choices.LensFocal },
                { "OSV_SS_FIT", Choices.DlogmCurve },
                { "OSV_SS_DEVICE", Choices.RenderDevice },
                { "OSV_SS_SIZE", Choices.SphereSize },
            };
            foreach (KeyValuePair<string, ChoiceList> kv in lists)
            {
                Check.Equal(kv.Value.Joined, d[kv.Key + "_ITEMS"], kv.Key + "_ITEMS");
                Check.Equal(long.Parse(d[kv.Key + "_COUNT"], CultureInfo.InvariantCulture), (long)kv.Value.Count, kv.Key + "_COUNT");
                // Colour Output's OpenFX default is its own (checked against OfxSourceParams.h).
                if (kv.Key != "OSV_SS_COLOR")
                {
                    Check.Equal(long.Parse(d[kv.Key + "_DEFAULT"], CultureInfo.InvariantCulture) - 1, (long)kv.Value.Default0,
                                kv.Key + "_DEFAULT (1-based) vs the 0-based OpenFX default");
                }
            }
            // The indices the extension names by hand.
            Check.Equal("Smooth + Horizon Lock", Choices.Stabilization[Choices.StabSmoothLevel], "StabSmoothLevel");
            Check.Equal("Full", Choices.Stabilization[Choices.StabFull], "StabFull");
            Check.Equal("Rec. 709", Choices.ColorOutput[Choices.ColorRec709], "ColorRec709");
            Check.Equal("BT.2100 PQ", Choices.ColorOutput[Choices.ColorPq], "ColorPq");
        }

        [Test]
        public static void CameraChoiceItemsMatchReframeParamsH()
        {
            Dictionary<string, string> d = CppHeader.Defines(Read("plugins/reframe/ReframeParams.h"));
            Check.Equal(Choices.Preset.Joined, d["OSV_REFRAME_PRESET_ITEMS"], "OSV_REFRAME_PRESET_ITEMS");
            Check.Equal(Choices.Lens.Joined, d["OSV_REFRAME_LENS_ITEMS"], "OSV_REFRAME_LENS_ITEMS");
            Check.Equal(Choices.KeyframeEasing.Joined, d["OSV_REFRAME_EASING_ITEMS"], "OSV_REFRAME_EASING_ITEMS");
            Check.Equal(long.Parse(d["OSV_REFRAME_PRESET_DEFAULT"], CultureInfo.InvariantCulture) - 1, (long)Choices.Preset.Default0, "preset default");
            Check.Equal(long.Parse(d["OSV_REFRAME_LENS_DEFAULT"], CultureInfo.InvariantCulture) - 1, (long)Choices.Lens.Default0, "lens default");
            Check.Equal(long.Parse(d["OSV_REFRAME_EASING_DEFAULT"], CultureInfo.InvariantCulture) - 1, (long)Choices.KeyframeEasing.Default0, "easing default");

            // Output Resolution: the shared list with its first entry renamed (OfxCamera.cpp).
            string[] items = d["OSV_REFRAME_RESOLUTION_ITEMS"].Split('|');
            items[0] = "Match Timeline";
            Check.Equal(string.Join("|", items), Choices.OutputResolution.Joined, "Output Resolution items");
            Check.Contains(Read("plugins/ofx/OfxCamera.cpp"), "items[0] = \"Match Timeline\"", "OfxCamera.cpp renames entry 0");

            // Every easing preset's label is its popup item.
            foreach (EasingPreset e in DjiCamera.Easings)
            {
                Check.Equal(e.Label, Choices.KeyframeEasing[e.Index0], "easing " + e.Id);
            }
        }

        /// <summary>DJI Studio's preset numbers, row by row from kPresetTable.</summary>
        [Test]
        public static void FramingLooksMatchThePresetTable()
        {
            string text = Read("plugins/reframe/ReframeParams.h");
            var re = new Regex(@"\{Preset::(\w+),\s*""([^""]+)"",\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*(true|false),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+),\s*([-\d.]+)\}");
            var rows = re.Matches(text).Cast<Match>().ToList();
            Check.Equal(5, rows.Count, "numeric kPresetTable rows (Custom uses macros and is skipped)");
            string[] presetItems = Choices.Preset.Joined.Split('|');
            foreach (Match row in rows)
            {
                string label = row.Groups[2].Value;
                int index0 = Array.IndexOf(presetItems, label);
                FramingLook look = DjiCamera.Looks.FirstOrDefault(l => l.PresetIndex0 == index0);
                Check.NotNull(look, "a framing look for preset '" + label + "'");
                Func<int, double> num = g => double.Parse(row.Groups[g].Value, CultureInfo.InvariantCulture);
                Check.Near(num(3), look.ClassicFov, 1e-9, label + " classic fov");
                Check.Near(num(4), look.Distortion, 1e-9, label + " distortion");
                Check.Near(num(5), look.Tilt, 1e-9, label + " tilt");
                Check.Equal("true", row.Groups[6].Value, label + " writes controls");
                Check.Near(num(7), look.DjiFovLandscape, 1e-9, label + " DJI fov landscape");
                Check.Near(num(8), look.DjiFovPortrait916, 1e-9, label + " DJI fov 9:16");
                Check.Near(num(9), look.DjiFovPortrait34, 1e-9, label + " DJI fov 3:4");
                Check.Near(num(10), look.Correction, 1e-9, label + " correction");
            }
        }

        /// <summary>The DJI zoom constants the default Zoom read-out is derived from.</summary>
        [Test]
        public static void DefaultZoomReadoutMatchesTheHeader()
        {
            Dictionary<string, string> d = CppHeader.Defines(Read("plugins/reframe/ReframeParams.h"));
            double fov = double.Parse(d["OSV_REFRAME_DJI_FOV_DEFAULT"], CultureInfo.InvariantCulture);
            double correction = double.Parse(d["OSV_REFRAME_CORRECTION_DEFAULT"], CultureInfo.InvariantCulture);
            double zoomDefault = double.Parse(d["OSV_REFRAME_ZOOM_DEFAULT"], CultureInfo.InvariantCulture);
            // "the value of the default DJI FOV / Correction on a 16:9 frame, rounded to the slider's tenths"
            double zoom = DjiCamera.ZoomDeg(fov, correction, 16.0 / 9.0);
            Check.Near(zoomDefault, Math.Round(zoom * 10.0) / 10.0, 1e-9, "default zoom read-out");
        }
    }
}
