// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AppPaths.cs - every folder and file the VEGAS extension writes to.

using System;
using System.IO;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// The per-user locations of the extension's log, settings and audio cache.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Local data (the log, the extracted audio) lives under
    /// <c>%LOCALAPPDATA%\OpenOSV\</c>, next to the OpenFX bundle's own
    /// <c>OpenOSVOfx.log</c>, so one folder holds everything a bug report needs.
    /// Settings roam with the user: <c>%APPDATA%\OpenOSV\vegas.json</c>.
    /// </para>
    /// <para>
    /// The test runner redirects both roots into a scratch folder with
    /// <see cref="OverrideRoots"/> before anything is written, so a test run
    /// never touches the user's real log, settings or cache.
    /// </para>
    /// </remarks>
    public static class AppPaths
    {
        /// <summary>Guards the two override fields.</summary>
        private static readonly object Gate = new object();

        /// <summary>Test override for the local root (null = the real one).</summary>
        private static string _localOverride;

        /// <summary>Test override for the roaming root (null = the real one).</summary>
        private static string _roamingOverride;

        /// <summary>Name of the product folder under both roots.</summary>
        public const string ProductFolder = "OpenOSV";

        /// <summary>
        /// Redirect both roots (tests only).  Pass nulls to restore the real
        /// locations.
        /// </summary>
        /// <param name="localRoot">Replaces <c>%LOCALAPPDATA%\OpenOSV</c>.</param>
        /// <param name="roamingRoot">Replaces <c>%APPDATA%\OpenOSV</c>.</param>
        public static void OverrideRoots(string localRoot, string roamingRoot)
        {
            lock (Gate)
            {
                _localOverride = string.IsNullOrWhiteSpace(localRoot) ? null : localRoot;
                _roamingOverride = string.IsNullOrWhiteSpace(roamingRoot) ? null : roamingRoot;
            }
        }

        /// <summary><c>%LOCALAPPDATA%\OpenOSV</c> (or the test override).</summary>
        public static string LocalDir
        {
            get
            {
                lock (Gate)
                {
                    if (_localOverride != null)
                    {
                        return _localOverride;
                    }
                }
                return Path.Combine(SpecialFolderOrTemp(Environment.SpecialFolder.LocalApplicationData), ProductFolder);
            }
        }

        /// <summary><c>%APPDATA%\OpenOSV</c> (or the test override).</summary>
        public static string RoamingDir
        {
            get
            {
                lock (Gate)
                {
                    if (_roamingOverride != null)
                    {
                        return _roamingOverride;
                    }
                }
                return Path.Combine(SpecialFolderOrTemp(Environment.SpecialFolder.ApplicationData), ProductFolder);
            }
        }

        /// <summary>The extension's rotating log file.</summary>
        public static string LogFile => Path.Combine(LocalDir, "OpenOSVVegas.log");

        /// <summary>Where extracted clip audio is cached (see <see cref="AudioCache"/>).</summary>
        public static string AudioCacheDir => Path.Combine(LocalDir, "vegas-audio");

        /// <summary>The extension's settings file.</summary>
        public static string SettingsFile => Path.Combine(RoamingDir, "vegas.json");

        /// <summary>Where the smoke-test script writes its report.</summary>
        public static string SmokeReportFile => Path.Combine(LocalDir, "vegas-smoke-report.txt");

        /// <summary>
        /// A special folder, or the temp folder when Windows has none to give
        /// (a service account, a broken profile) - never an empty string that
        /// would turn every path relative to VEGAS's working directory.
        /// </summary>
        private static string SpecialFolderOrTemp(Environment.SpecialFolder folder)
        {
            try
            {
                string path = Environment.GetFolderPath(folder);
                if (!string.IsNullOrWhiteSpace(path))
                {
                    return path;
                }
            }
            catch (Exception)
            {
                // Fall through to the temp folder.
            }
            return Path.GetTempPath();
        }

        /// <summary>
        /// Create a directory if it is missing.  Returns false (never throws)
        /// when it cannot be created.
        /// </summary>
        /// <param name="dir">The directory.</param>
        public static bool EnsureDir(string dir)
        {
            if (string.IsNullOrWhiteSpace(dir))
            {
                return false;
            }
            try
            {
                Directory.CreateDirectory(dir);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
