// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Session.cs - the extension's per-VEGAS-session state: settings, osvtool,
// the probe and audio caches.

using System;
using System.Threading;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.Host
{
    /// <summary>State shared by every command and the panel for one VEGAS session.</summary>
    internal static class Session
    {
        private static Settings _settings;
        private static OsvToolLocation _tool;

        /// <summary>The user's settings (loaded on first use).</summary>
        public static Settings Settings
        {
            get
            {
                if (_settings is null)
                {
                    _settings = Settings.Load();
                }
                return _settings;
            }
        }

        /// <summary>Save the settings (logged, never throws).</summary>
        public static void SaveSettings()
        {
            if (_settings is not null && !_settings.Save())
            {
                Log.Warn("session: the settings could not be saved");
            }
        }

        /// <summary>Probe results for this session.</summary>
        public static ProbeCache Probes { get; } = new ProbeCache();

        /// <summary>The extracted-audio cache.</summary>
        public static AudioCache Audio { get; } = new AudioCache(AppPaths.AudioCacheDir);

        /// <summary>Where osvtool was found (looked up once, again after <see cref="ForgetTool"/>).</summary>
        public static OsvToolLocation ToolLocation
        {
            get
            {
                if (_tool is null || (_tool.Found && !SiblingFiles.Exists(_tool.ExePath)))
                {
                    _tool = OsvTool.Locate(Settings.OsvToolPath);
                    Log.Info(_tool.Found
                        ? "session: osvtool is " + _tool.ExePath + " (" + _tool.Source + ")"
                        : "session: osvtool not found; looked in " + string.Join("; ", _tool.Searched));
                }
                return _tool;
            }
        }

        /// <summary>The osvtool to run, or null when there is none.</summary>
        public static OsvTool Tool => ToolLocation.Found ? new OsvTool(ToolLocation.ExePath) : null;

        /// <summary>Look osvtool up again (after the user picked one).</summary>
        public static void ForgetTool()
        {
            _tool = null;
            Probes.Clear();
        }

        /// <summary>Probe a clip through the session cache.</summary>
        public static ProbeResult Probe(string path, CancellationToken cancel, out string error)
        {
            OsvTool tool = Tool;
            if (tool is null)
            {
                error = "osvtool was not found";
                return null;
            }
            return Probes.Get(tool, path, cancel, out error);
        }
    }
}
