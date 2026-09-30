// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OpenOSV Smoke Test.cs - a VEGAS Pro script that checks, in a live VEGAS,
// every scripting-API fact the OpenOSV extension relies on.
//
// Run it from Tools > Scripting > Run Script... with a project open.  It asks
// for one .OSV (or .LRF), imports it the way the extension does - generated
// media, exact length, frame size, parameters, extracted audio, one group -
// then reads everything back and writes a report:
//
//     %LOCALAPPDATA%\OpenOSV\vegas-smoke-report.txt
//
// Everything it adds is one undo step ("OpenOSV smoke test"): Ctrl+Z removes
// it again.  VEGAS compiles scripts itself, so this file is C# 5 on purpose
// (checked with csc -langversion:5 against VEGAS 16's and 17's scripting
// API): no string interpolation, no ?. operator, no expression bodies,
// nothing newer.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Text;
using System.Text.RegularExpressions;
using System.Windows.Forms;
using ScriptPortal.Vegas;

public class EntryPoint
{
    private readonly StringBuilder _report = new StringBuilder();
    private int _passed;
    private int _failed;
    private int _notes;

    private const string SourceId = "org.openosv.OSVSource";
    private const string ReframeId = "org.openosv.Open360Reframe";

    public void FromVegas(Vegas vegas)
    {
        string reportPath = Path.Combine(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "OpenOSV"), "vegas-smoke-report.txt");
        try
        {
            Line("OpenOSV smoke test - " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
            Line("VEGAS " + vegas.Version + " (" + vegas.LongAppName + ")");
            Run(vegas);
        }
        catch (Exception ex)
        {
            Fail("the smoke test itself", ex.ToString());
        }

        // ---- the report ------------------------------------------------------------------
        Line("");
        Line(_passed + " passed, " + _failed + " failed, " + _notes + " notes");
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(reportPath));
            File.WriteAllText(reportPath, _report.ToString(), new UTF8Encoding(false));
        }
        catch (Exception ex)
        {
            MessageBox.Show("Could not write the report: " + ex.Message, "OpenOSV smoke test");
        }
        MessageBox.Show(
            (_failed == 0 ? "All checks passed." : _failed + " check(s) failed.") + "\n\n" +
            _passed + " passed, " + _failed + " failed, " + _notes + " notes.\n\nReport: " + reportPath +
            "\n\nCtrl+Z removes what the test added.",
            "OpenOSV smoke test", MessageBoxButtons.OK, _failed == 0 ? MessageBoxIcon.Information : MessageBoxIcon.Warning);
    }

    private void Run(Vegas vegas)
    {
        Project project = vegas.Project;
        if (project == null)
        {
            Fail("a project is open", "open or create a project first");
            return;
        }

        // =====================================================================
        //  1. The environment
        // =====================================================================
        Section("Environment");
        string pixelFormat = EnumName(project.Video, "PixelFormat");
        Note("project " + project.Video.Width + " x " + project.Video.Height + " at " + project.Video.FrameRate.ToString("0.###") +
             " fps, pixel format " + (pixelFormat ?? "(unknown)") + ", HDR " + (EnumName(project.Video, "HDRMode") ?? "(no setting)") +
             ", 360 output " + (EnumName(project.Video, "T360Output") ?? "(no setting)"));
        Assembly extension = null;
        foreach (Assembly a in AppDomain.CurrentDomain.GetAssemblies())
        {
            if (string.Equals(a.GetName().Name, "OpenOSV.Vegas", StringComparison.OrdinalIgnoreCase))
            {
                extension = a;
            }
        }
        if (extension != null)
        {
            Pass("the OpenOSV extension is loaded", extension.GetName().Version + " from " + SafeLocation(extension));
        }
        else
        {
            Note("the OpenOSV extension is not loaded (the rest of the test does not need it)");
        }
        long unitsPerSecond = Timecode.FromMilliseconds(1000.0).Nanos;
        Note("Timecode units per second: " + unitsPerSecond);

        // =====================================================================
        //  2. The plug-ins as VEGAS lists them
        // =====================================================================
        Section("Plug-ins");
        PlugInNode generator = null;
        try
        {
            generator = vegas.Generators.FindChildByUniqueID("{Svfx:" + SourceId + "}");
        }
        catch (Exception)
        {
            generator = null;
        }
        List<PlugInNode> generators = FindAll(vegas.Generators, SourceId);
        List<PlugInNode> filters = FindAll(vegas.VideoFX, ReframeId);
        foreach (PlugInNode n in generators)
        {
            Note("generator entry: '" + n.Name + "' unique id " + n.UniqueID);
        }
        foreach (PlugInNode n in filters)
        {
            Note("video FX entry: '" + n.Name + "' unique id " + n.UniqueID);
        }
        if (generator == null && generators.Count > 0)
        {
            generator = generators[0];
        }
        Check(generator != null, "OpenOSV Source is listed under {Svfx:" + SourceId + "}",
              generator != null ? generator.UniqueID : "install the OpenOSV OpenFX bundle and restart VEGAS");
        Check(filters.Count > 0, "OpenOSV 360 Reframe is listed", filters.Count + " entr" + (filters.Count == 1 ? "y" : "ies"));
        if (generator == null)
        {
            return;
        }

        // =====================================================================
        //  3. osvtool and the clip
        // =====================================================================
        Section("osvtool");
        string osvtool = FindOsvTool();
        if (osvtool == null)
        {
            OpenFileDialog pick = new OpenFileDialog();
            pick.Title = "Where is osvtool.exe?";
            pick.Filter = "osvtool|osvtool.exe";
            if (pick.ShowDialog() == DialogResult.OK)
            {
                osvtool = pick.FileName;
            }
        }
        Check(osvtool != null && File.Exists(osvtool), "osvtool found", osvtool ?? "none");
        if (osvtool == null)
        {
            return;
        }
        OpenFileDialog dialog = new OpenFileDialog();
        dialog.Title = "OpenOSV smoke test: pick an .OSV (or .LRF)";
        dialog.Filter = "DJI Osmo 360 clips (*.osv;*.lrf)|*.osv;*.lrf";
        if (dialog.ShowDialog() != DialogResult.OK)
        {
            Note("no clip chosen; stopped");
            return;
        }
        string clip = dialog.FileName;
        Note("clip: " + clip);

        string temp = Path.Combine(Path.GetTempPath(), "openosv-smoke-" + Guid.NewGuid().ToString("N").Substring(0, 8));
        Directory.CreateDirectory(temp);
        string jsonPath = Path.Combine(temp, "probe.json");
        string stdout;
        string stderr;
        int exit = RunTool(osvtool, "probe \"" + clip + "\" --json \"" + jsonPath + "\"", temp, out stdout, out stderr);
        Check(exit == 0 && File.Exists(jsonPath), "osvtool probe", "exit " + exit + " " + Trim(stderr));
        if (!File.Exists(jsonPath))
        {
            return;
        }
        string json = File.ReadAllText(jsonPath);
        long frames = FirstLong(json, "\"frameCount\"\\s*:\\s*(\\d+)");
        long num = FirstLong(json, "\"fps\"\\s*:\\s*\\{[^}]*?\"num\"\\s*:\\s*(\\d+)");
        long den = FirstLong(json, "\"fps\"\\s*:\\s*\\{[^}]*?\"den\"\\s*:\\s*(\\d+)");
        Check(frames > 0 && num > 0 && den > 0, "the probe states frames and a rational rate (schema openosv.probe/1)",
              frames + " frames at " + num + "/" + den);
        if (frames <= 0 || num <= 0 || den <= 0)
        {
            Note("this osvtool predates the flat probe summary: update the OpenFX bundle");
            return;
        }

        // =====================================================================
        //  4. The generated media, as the extension builds it
        // =====================================================================
        Section("Generated media");
        long expectedUnits = (long)Math.Round((decimal)frames * den * unitsPerSecond / num, MidpointRounding.AwayFromZero);
        Timecode length = Timecode.FromNanos(expectedUnits);
        int width = project.Video.Width;
        int height = project.Video.Height;
        string levelsWanted = (pixelFormat != null && pixelFormat.IndexOf("FullRange", StringComparison.OrdinalIgnoreCase) >= 0)
            ? "Full range (0-255)" : "Studio RGB (16-235)";

        using (new UndoBlock(project, "OpenOSV smoke test"))
        {
            Media media = new Media(generator);
            Check(media.IsGenerated(), "new Media(generator) makes generated media", media.KeyString);
            Effect effect = media.Generator;
            OFXEffect fx = effect != null && effect.IsOFX ? effect.OFXEffect : null;
            Check(fx != null, "the media's generator is an OpenFX effect", effect != null ? effect.PlugIn.UniqueID : "no generator");
            if (fx == null)
            {
                return;
            }

            // ---- parameters, each followed by ParameterChanged -----------------------------
            OFXStringParameter file = fx.FindParameterByName("file") as OFXStringParameter;
            Check(file != null, "parameter 'file' is a string", "");
            if (file != null)
            {
                file.Value = clip;
                file.ParameterChanged();
            }
            Check(SetChoice(fx, "output", 0, "Reframed view"), "parameter 'output' set to Reframed view", "");
            bool hasLevels = fx.FindParameterByName("outputLevels") != null;
            if (hasLevels)
            {
                Check(SetChoice(fx, "outputLevels", levelsWanted.StartsWith("Full") ? 0 : 1, levelsWanted), "parameter 'outputLevels' set to " + levelsWanted, "");
            }
            else
            {
                Note("no 'outputLevels' parameter: this OpenFX bundle predates VEGAS support");
            }

            // ---- length and size -----------------------------------------------------------------------
            media.Length = length;
            VideoStream stream = media.GetVideoStreamByIndex(0);
            Check(stream != null, "the generated media has a video stream", "");
            if (stream == null)
            {
                return;
            }
            stream.Size = new Size(width, height);
            media.Comment = clip;
            Check(media.Length.Nanos == expectedUnits, "Media.Length is exact", media.Length.Nanos + " units, expected " + expectedUnits);
            Check(stream.Size.Width == width && stream.Size.Height == height, "VideoStream.Size is settable",
                  stream.Size.Width + " x " + stream.Size.Height);

            // ---- read-backs that answer the open questions -----------------------------------------------
            Check(file != null && file.Value == clip, "the file parameter reads back", file != null ? file.Value : "");
            OFXStringParameter clipInfo = fx.FindParameterByName("clipInfo") as OFXStringParameter;
            string info = clipInfo != null ? clipInfo.Value : "(no clipInfo)";
            if (info.IndexOf("frames at", StringComparison.Ordinal) >= 0)
            {
                Pass("ParameterChanged reached the plug-in (its Clip read-out was refreshed)", info);
            }
            else
            {
                Note("the Clip read-out says '" + info + "': VEGAS may not send instance-changed for scripted edits (the extension writes every value itself, so this is informational)");
            }
            try
            {
                Guid id = new Guid("6f0b3c1e-5a57-4b8e-9a3d-0e6c1f2a7d41");
                media.CustomData.SetBytes(id, Encoding.UTF8.GetBytes("{\"v\":1}"));
                byte[] back = media.CustomData.GetBytes(id);
                Check(back != null && Encoding.UTF8.GetString(back) == "{\"v\":1}", "Media.CustomData round-trips in the session", "");
                Note("CustomData persistence across save/reopen is not checked here: save, reopen, and look at an imported clip's proxy toggle");
            }
            catch (Exception ex)
            {
                Fail("Media.CustomData", ex.Message);
            }

            // ---- events and audio ------------------------------------------------------------------------
            Section("Events, audio, group");
            Timecode start = vegas.Transport.CursorPosition;
            VideoTrack videoTrack = project.AddVideoTrack();
            videoTrack.Name = "OpenOSV smoke test";
            VideoEvent video = videoTrack.AddVideoEvent(start, length);
            video.AddTake(stream, true, Path.GetFileName(clip));
            Check(video.Length.Nanos == expectedUnits, "the video event is as long as the media", video.Length.ToString());

            string wav = Path.Combine(temp, "audio.wav");
            exit = RunTool(osvtool, "extract \"" + clip + "\" --audio \"" + wav + "\"", temp, out stdout, out stderr);
            bool isWav = File.Exists(wav) && IsRiffWave(wav);
            Check(exit == 0 && isWav, "osvtool extract --audio writes a WAV", "exit " + exit + (isWav ? "" : " (not RIFF/WAVE: an osvtool without WAV support)"));
            if (isWav)
            {
                Media audioMedia = project.MediaPool.AddMedia(wav);
                AudioStream audioStream = audioMedia != null ? audioMedia.GetAudioStreamByIndex(0) : null;
                Check(audioStream != null, "VEGAS opens the extracted WAV",
                      audioStream != null ? audioStream.SampleRate + " Hz, " + audioStream.Channels + " ch, " + audioStream.Length : "");
                if (audioStream != null)
                {
                    AudioTrack audioTrack = project.AddAudioTrack();
                    audioTrack.Name = "OpenOSV smoke test audio";
                    Timecode audioLength = audioStream.Length.Nanos < length.Nanos ? audioStream.Length : length;
                    AudioEvent audio = audioTrack.AddAudioEvent(start, audioLength);
                    audio.AddTake(audioStream, true, Path.GetFileName(clip));
                    Note("audio stream " + audioStream.Length.Nanos + " units vs video " + length.Nanos + " units (difference " +
                         (audioStream.Length.Nanos - length.Nanos) + ")");

                    TrackEventGroup group = new TrackEventGroup(project);
                    project.TrackEventGroups.Add(group);
                    group.Add(video);
                    group.Add(audio);
                    Check(video.IsGrouped && audio.IsGrouped && group.Count == 2, "video and audio are grouped", "group of " + group.Count);
                }
            }

            // ---- pan/crop and the filter's position (what Apply 360 Reframe changes) -----------------------------
            Section("Pan/crop");
            try
            {
                video.MaintainAspectRatio = false;
                Pass("VideoEvent.MaintainAspectRatio is settable", video.MaintainAspectRatio.ToString());
                VideoMotionKeyframe k = video.VideoMotion.Keyframes[0];
                Note("pan/crop keyframe 0 bounds: " + k.Bounds.TopLeft + " .. " + k.Bounds.BottomRight + " (source " + stream.Size.Width + " x " + stream.Size.Height + ")");
            }
            catch (Exception ex)
            {
                Fail("pan/crop access", ex.Message);
            }
        }
        Note("everything above is one undo step: Ctrl+Z removes it");
    }

    // =========================================================================
    //  Helpers
    // =========================================================================

    private static bool SetChoice(OFXEffect fx, string name, int index, string label)
    {
        OFXChoiceParameter p = fx.FindParameterByName(name) as OFXChoiceParameter;
        if (p == null)
        {
            return false;
        }
        OFXChoice chosen = null;
        foreach (OFXChoice c in p.Choices)
        {
            if (c != null && (c.Name == label || (chosen == null && c.Index == index)))
            {
                chosen = c;
            }
        }
        if (chosen == null)
        {
            return false;
        }
        p.Value = chosen;
        p.ParameterChanged();
        return p.Value != null && p.Value.Index == chosen.Index;
    }

    private static List<PlugInNode> FindAll(PlugInNode root, string ofxId)
    {
        List<PlugInNode> found = new List<PlugInNode>();
        Stack<PlugInNode> stack = new Stack<PlugInNode>();
        if (root != null)
        {
            stack.Push(root);
        }
        int guard = 0;
        while (stack.Count > 0 && guard++ < 20000)
        {
            PlugInNode node = stack.Pop();
            foreach (PlugInNode child in node)
            {
                if (child == null)
                {
                    continue;
                }
                if (child.IsContainer)
                {
                    stack.Push(child);
                }
                else if (child.UniqueID != null && child.UniqueID.IndexOf(ofxId, StringComparison.OrdinalIgnoreCase) >= 0)
                {
                    found.Add(child);
                }
            }
        }
        return found;
    }

    private static string FindOsvTool()
    {
        foreach (string variable in new string[] { "CommonProgramW6432", "CommonProgramFiles" })
        {
            string root = Environment.GetEnvironmentVariable(variable);
            if (string.IsNullOrEmpty(root))
            {
                continue;
            }
            string candidate = Path.Combine(root, @"OFX\Plugins\OpenOSV.ofx.bundle\Contents\Libraries\Win64\osvtool.exe");
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }
        string path = Environment.GetEnvironmentVariable("PATH") ?? "";
        foreach (string dir in path.Split(';'))
        {
            try
            {
                string candidate = Path.Combine(dir.Trim().Trim('"'), "osvtool.exe");
                if (dir.Trim().Length > 0 && File.Exists(candidate))
                {
                    return candidate;
                }
            }
            catch (ArgumentException)
            {
                // A malformed PATH entry.
            }
        }
        return null;
    }

    private static int RunTool(string exe, string arguments, string workingDirectory, out string stdout, out string stderr)
    {
        ProcessStartInfo psi = new ProcessStartInfo(exe, arguments);
        psi.UseShellExecute = false;
        psi.CreateNoWindow = true;
        psi.RedirectStandardOutput = true;
        psi.RedirectStandardError = true;
        psi.WorkingDirectory = workingDirectory;
        using (Process p = Process.Start(psi))
        {
            // Read stderr on its own thread so a full pipe can never block.
            string err = "";
            System.Threading.Thread t = new System.Threading.Thread(delegate () { err = p.StandardError.ReadToEnd(); });
            t.Start();
            stdout = p.StandardOutput.ReadToEnd();
            if (!p.WaitForExit(600000))
            {
                p.Kill();
            }
            t.Join(5000);
            stderr = err;
            return p.HasExited ? p.ExitCode : -1;
        }
    }

    private static bool IsRiffWave(string path)
    {
        byte[] head = new byte[12];
        using (FileStream fs = File.OpenRead(path))
        {
            if (fs.Read(head, 0, 12) < 12)
            {
                return false;
            }
        }
        string riff = Encoding.ASCII.GetString(head, 0, 4);
        return (riff == "RIFF" || riff == "RF64") && Encoding.ASCII.GetString(head, 8, 4) == "WAVE";
    }

    private static long FirstLong(string text, string pattern)
    {
        Match m = Regex.Match(text, pattern, RegexOptions.Singleline);
        long value;
        return m.Success && long.TryParse(m.Groups[1].Value, out value) ? value : 0;
    }

    private static string EnumName(object target, string property)
    {
        try
        {
            PropertyInfo p = target.GetType().GetProperty(property);
            object v = p != null ? p.GetValue(target, null) : null;
            return v != null ? v.ToString() : null;
        }
        catch (Exception)
        {
            return null;
        }
    }

    private static string SafeLocation(Assembly a)
    {
        try
        {
            return a.Location;
        }
        catch (Exception)
        {
            return "(unknown)";
        }
    }

    private static string Trim(string s)
    {
        if (string.IsNullOrEmpty(s))
        {
            return "";
        }
        s = s.Trim();
        return s.Length > 300 ? s.Substring(s.Length - 300) : s;
    }

    private void Section(string title)
    {
        Line("");
        Line("== " + title);
    }

    private void Check(bool ok, string what, string detail)
    {
        if (ok)
        {
            Pass(what, detail);
        }
        else
        {
            Fail(what, detail);
        }
    }

    private void Pass(string what, string detail)
    {
        _passed++;
        Line("  PASS  " + what + (string.IsNullOrEmpty(detail) ? "" : " - " + detail));
    }

    private void Fail(string what, string detail)
    {
        _failed++;
        Line("  FAIL  " + what + (string.IsNullOrEmpty(detail) ? "" : " - " + detail));
    }

    private void Note(string text)
    {
        _notes++;
        Line("  note  " + text);
    }

    private void Line(string text)
    {
        _report.Append(text).Append("\r\n");
    }
}
