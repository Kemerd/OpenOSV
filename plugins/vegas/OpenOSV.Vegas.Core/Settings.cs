// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Settings.cs - the extension's remembered choices, %APPDATA%\OpenOSV\vegas.json.

using System;
using System.IO;
using System.Text;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// Everything the extension remembers between VEGAS sessions.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Stored as small, human-readable JSON so a user can fix a bad path by
    /// hand.  Reading is forgiving: a missing file, a missing key, a value of
    /// the wrong type or a corrupt file all fall back to the defaults (a
    /// corrupt file is kept as <c>vegas.json.bad</c> for a bug report).
    /// Writing is atomic: the new text goes to a temporary file that replaces
    /// the old one in a single rename, so a crash mid-save never leaves half a
    /// file.
    /// </para>
    /// </remarks>
    public sealed class Settings
    {
        /// <summary>Format version written into the file.</summary>
        public const int CurrentVersion = 1;

        /// <summary>osvtool.exe chosen by the user (empty = find it automatically).</summary>
        public string OsvToolPath { get; set; } = string.Empty;

        /// <summary>The folder the Import dialog opened last.</summary>
        public string LastImportFolder { get; set; } = string.Empty;

        /// <summary>The folder Relink searched last.</summary>
        public string LastRelinkFolder { get; set; } = string.Empty;

        /// <summary>Output for new imports: 0 = Reframed view, 1 = 360 equirect.</summary>
        public int DefaultOutput { get; set; } = Choices.OutputReframed;

        /// <summary>DJI Studio's RockSteady switch for new imports and the Stabilisation buttons.</summary>
        public bool RockSteady { get; set; } = true;

        /// <summary>DJI Studio's Horizon Leveling switch.</summary>
        public bool HorizonLeveling { get; set; } = true;

        /// <summary>The last framing look applied (a <see cref="FramingLook.Id"/>).</summary>
        public string LastLook { get; set; } = "wide";

        /// <summary>The last easing preset applied (an <see cref="EasingPreset.Id"/>).</summary>
        public string LastEasing { get; set; } = "none";

        /// <summary>Extract each clip's audio on import.</summary>
        public bool ExtractAudio { get; set; } = true;

        /// <summary>Use AAC when the installed osvtool cannot write WAV.</summary>
        public bool AllowAacFallback { get; set; } = true;

        /// <summary>The panel's "How it works" card is open (true until the user folds it once).</summary>
        public bool HelpOpen { get; set; } = true;

        /// <summary>Load the settings from <see cref="AppPaths.SettingsFile"/>.  Never throws.</summary>
        public static Settings Load() => Load(AppPaths.SettingsFile);

        /// <summary>Load settings from a file.  Never throws.</summary>
        public static Settings Load(string path)
        {
            var s = new Settings();
            string text;
            try
            {
                if (string.IsNullOrWhiteSpace(path) || !File.Exists(path))
                {
                    return s;
                }
                text = File.ReadAllText(path, Encoding.UTF8);
            }
            catch (Exception ex)
            {
                Log.Warn("settings: cannot read '" + path + "'; using defaults", ex);
                return s;
            }
            if (!Json.TryParse(text, out JsonValue doc, out string error) || !doc.IsObject)
            {
                Log.Warn("settings: '" + path + "' is not valid JSON (" + (error ?? "not an object") + "); kept as .bad, using defaults");
                try
                {
                    File.Copy(path, path + ".bad", true);
                }
                catch (Exception)
                {
                    // Keeping the bad copy is a courtesy, not a requirement.
                }
                return s;
            }

            // ---- every key optional, every type checked --------------------------------
            s.OsvToolPath = doc["osvtoolPath"].AsString(s.OsvToolPath) ?? string.Empty;
            s.LastImportFolder = doc["lastImportFolder"].AsString(s.LastImportFolder) ?? string.Empty;
            s.LastRelinkFolder = doc["lastRelinkFolder"].AsString(s.LastRelinkFolder) ?? string.Empty;
            int output = doc["defaultOutput"].AsInt(s.DefaultOutput);
            s.DefaultOutput = (output == Choices.OutputEquirect) ? Choices.OutputEquirect : Choices.OutputReframed;
            s.RockSteady = doc["rockSteady"].AsBool(s.RockSteady);
            s.HorizonLeveling = doc["horizonLeveling"].AsBool(s.HorizonLeveling);
            string look = doc["lastLook"].AsString(s.LastLook);
            s.LastLook = DjiCamera.LookById(look) != null ? look : "wide";
            string easing = doc["lastEasing"].AsString(s.LastEasing);
            s.LastEasing = DjiCamera.EasingById(easing) != null ? easing : "none";
            s.ExtractAudio = doc["extractAudio"].AsBool(s.ExtractAudio);
            s.AllowAacFallback = doc["allowAacFallback"].AsBool(s.AllowAacFallback);
            s.HelpOpen = doc["helpOpen"].AsBool(s.HelpOpen);
            return s;
        }

        /// <summary>The settings as JSON text.</summary>
        public string ToJson()
        {
            var w = new JsonWriter();
            w.BeginObject();
            w.Value("version", CurrentVersion);
            w.Value("osvtoolPath", OsvToolPath ?? string.Empty);
            w.Value("lastImportFolder", LastImportFolder ?? string.Empty);
            w.Value("lastRelinkFolder", LastRelinkFolder ?? string.Empty);
            w.Value("defaultOutput", DefaultOutput);
            w.Value("rockSteady", RockSteady);
            w.Value("horizonLeveling", HorizonLeveling);
            w.Value("lastLook", LastLook ?? "wide");
            w.Value("lastEasing", LastEasing ?? "none");
            w.Value("extractAudio", ExtractAudio);
            w.Value("allowAacFallback", AllowAacFallback);
            w.Value("helpOpen", HelpOpen);
            w.EndObject();
            return w.ToString() + "\n";
        }

        /// <summary>Save to <see cref="AppPaths.SettingsFile"/>.  Returns false (never throws) on failure.</summary>
        public bool Save() => Save(AppPaths.SettingsFile);

        /// <summary>Save to a file, atomically.  Returns false (never throws) on failure.</summary>
        public bool Save(string path)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                if (!AppPaths.EnsureDir(dir))
                {
                    return false;
                }
                string temp = path + ".tmp";
                File.WriteAllText(temp, ToJson(), new UTF8Encoding(false));
                if (File.Exists(path))
                {
                    File.Replace(temp, path, null, true);
                }
                else
                {
                    File.Move(temp, path);
                }
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("settings: cannot save '" + path + "'", ex);
                return false;
            }
        }
    }
}
