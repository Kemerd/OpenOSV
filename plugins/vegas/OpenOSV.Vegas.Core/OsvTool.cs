// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvTool.cs - finding osvtool.exe and asking it about clips.
//
// ===========================================================================
//  Why osvtool, not a library
// ===========================================================================
// The extension runs inside VEGAS's process.  Loading the clip engine (FFmpeg,
// CUDA, the whole importer) into it a second time, beside the OpenFX bundle's
// copy, would be asking for DLL conflicts - and a crash there takes the user's
// session with it.  osvtool is the same engine in its own process: if it
// fails, the extension reports a line and VEGAS carries on.  The OpenFX
// bundle ships it (Contents\Libraries\Win64\osvtool.exe).
//
// ===========================================================================
//  Old and new osvtool
// ===========================================================================
//   probe <file> --json -       new: the document on stdout.  An older build
//                               writes a FILE named "-" in its working
//                               directory instead, so every run gets a fresh
//                               scratch directory and the fallback below.
//   probe <file> --json <path>  every build: the document in a file.
//   extract <file> --audio x.wav  new: a sync-exact 32-bit float WAV.  An
//                               older build writes raw AAC (ADTS) whatever the
//                               extension says, so the output's header is
//                               checked; AAC in a .wav is deleted and reported
//                               as "no WAV support" for the caller to decide.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Threading;

namespace OpenOSV.Vegas.Core
{
    /// <summary>Where an osvtool.exe was found.</summary>
    public enum OsvToolSource
    {
        /// <summary>The path the user chose in the panel.</summary>
        UserSetting,
        /// <summary>The OpenFX bundle's copy.</summary>
        OfxBundle,
        /// <summary>The PATH.</summary>
        Path,
    }

    /// <summary>The result of looking for osvtool.</summary>
    public sealed class OsvToolLocation
    {
        /// <summary>Full path of osvtool.exe, or null when none was found.</summary>
        public string ExePath { get; internal set; }

        /// <summary>Where it was found.</summary>
        public OsvToolSource Source { get; internal set; }

        /// <summary>Every place that was checked, in order (for the "not found" message).</summary>
        public List<string> Searched { get; } = new List<string>();

        /// <summary>True when a tool was found.</summary>
        public bool Found => !string.IsNullOrEmpty(ExePath);
    }

    /// <summary>The outcome of extracting a clip's audio.</summary>
    public sealed class AudioExtraction
    {
        /// <summary>The audio file to put on the timeline (null on failure).</summary>
        public string Path { get; internal set; }

        /// <summary>True for a WAV (false for the AAC fallback).</summary>
        public bool IsWav { get; internal set; }

        /// <summary>True when an earlier extraction was reused.</summary>
        public bool FromCache { get; internal set; }

        /// <summary>The WAV header, when it is a WAV.</summary>
        public WavInfo Wav { get; internal set; }

        /// <summary>Why it failed (null on success).</summary>
        public string Error { get; internal set; }

        /// <summary>True when this osvtool cannot write WAV (it wrote AAC instead).</summary>
        public bool WavUnsupported { get; internal set; }

        /// <summary>True when the run was cancelled.</summary>
        public bool Cancelled { get; internal set; }

        /// <summary>True when there is a file to use.</summary>
        public bool Succeeded => !string.IsNullOrEmpty(Path) && Error == null;
    }

    /// <summary>One osvtool.exe and the commands the extension runs with it.</summary>
    public sealed class OsvTool
    {
        /// <summary>The file name looked for everywhere.</summary>
        public const string ExeName = "osvtool.exe";

        /// <summary>How long a probe may take.</summary>
        public static readonly TimeSpan ProbeTimeout = TimeSpan.FromSeconds(90);

        /// <summary>Wrap an osvtool.exe path.</summary>
        public OsvTool(string exePath)
        {
            ExePath = exePath ?? string.Empty;
        }

        /// <summary>Full path of the executable.</summary>
        public string ExePath { get; }

        // =====================================================================
        //  Finding it
        // =====================================================================

        /// <summary>
        /// The OpenFX bundle's osvtool for this machine:
        /// <c>%CommonProgramFiles%\OFX\Plugins\OpenOSV.ofx.bundle\Contents\Libraries\Win64\osvtool.exe</c>.
        /// </summary>
        public static IEnumerable<string> BundleCandidates()
        {
            var roots = new List<string>();
            foreach (string variable in new[] { "CommonProgramW6432", "CommonProgramFiles" })
            {
                try
                {
                    string root = Environment.GetEnvironmentVariable(variable);
                    if (!string.IsNullOrWhiteSpace(root) && !roots.Contains(root, StringComparer.OrdinalIgnoreCase))
                    {
                        roots.Add(root);
                    }
                }
                catch (Exception)
                {
                    // Environment access denied: skip that root.
                }
            }
            foreach (string root in roots)
            {
                string contents = System.IO.Path.Combine(root, "OFX", "Plugins", "OpenOSV.ofx.bundle", "Contents");
                // The documented place first, then beside the plug-in itself,
                // where bundles from before the Libraries folder kept it.
                yield return System.IO.Path.Combine(contents, "Libraries", "Win64", ExeName);
                yield return System.IO.Path.Combine(contents, "Win64", ExeName);
            }
        }

        /// <summary>
        /// Find osvtool: the user's own choice when it still exists (an explicit
        /// choice beats every default), then the OpenFX bundle, then the PATH.
        /// Never throws.
        /// </summary>
        /// <param name="userPath">The path saved in the settings, or null.</param>
        public static OsvToolLocation Locate(string userPath)
        {
            var loc = new OsvToolLocation();

            // ---- 1. the user's choice ---------------------------------------------------
            if (!string.IsNullOrWhiteSpace(userPath))
            {
                loc.Searched.Add(userPath);
                if (IsExe(userPath))
                {
                    loc.ExePath = SiblingFiles.NormalizeOrSelf(userPath);
                    loc.Source = OsvToolSource.UserSetting;
                    return loc;
                }
            }

            // ---- 2. the OpenFX bundle ----------------------------------------------------------
            foreach (string candidate in BundleCandidates())
            {
                loc.Searched.Add(candidate);
                if (IsExe(candidate))
                {
                    loc.ExePath = candidate;
                    loc.Source = OsvToolSource.OfxBundle;
                    return loc;
                }
            }

            // ---- 3. the PATH -----------------------------------------------------------------------
            string pathVar = null;
            try
            {
                pathVar = Environment.GetEnvironmentVariable("PATH");
            }
            catch (Exception)
            {
                // No PATH to search.
            }
            loc.Searched.Add("PATH");
            foreach (string dir in (pathVar ?? string.Empty).Split(';'))
            {
                string d = dir.Trim().Trim('"');
                if (d.Length == 0)
                {
                    continue;
                }
                string candidate;
                try
                {
                    candidate = System.IO.Path.Combine(d, ExeName);
                }
                catch (ArgumentException)
                {
                    continue;
                }
                if (IsExe(candidate))
                {
                    loc.ExePath = candidate;
                    loc.Source = OsvToolSource.Path;
                    return loc;
                }
            }
            return loc;
        }

        private static bool IsExe(string path)
        {
            try
            {
                return !string.IsNullOrWhiteSpace(path) &&
                       path.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) &&
                       File.Exists(path);
            }
            catch (Exception)
            {
                return false;
            }
        }

        // =====================================================================
        //  probe
        // =====================================================================

        /// <summary>
        /// Probe a clip.  Tries the document on stdout first and falls back to a
        /// temporary --json file (older osvtool).  Returns null with
        /// <paramref name="error"/> set on failure.  Never throws.
        /// </summary>
        public ProbeResult Probe(string clipPath, CancellationToken cancel, out string error)
        {
            error = null;
            if (!SiblingFiles.Exists(clipPath))
            {
                error = "the file is missing: " + clipPath;
                return null;
            }
            string scratch = MakeScratchDir();
            try
            {
                string input = CommandLine.AsciiSafePath(clipPath);

                // ---- 1. the document on stdout (current osvtool) -------------------------
                ProcessOutcome run = ProcessRunner.Run(ExePath, new[] { "probe", input, "--json", "-" }, scratch, ProbeTimeout, cancel);
                if (run.Cancelled)
                {
                    error = "cancelled";
                    return null;
                }
                if (run.Succeeded)
                {
                    ProbeResult fromStdout = ProbeResult.Parse(run.StdOut, clipPath, out string parseError);
                    if (fromStdout != null)
                    {
                        return fromStdout;
                    }
                    // An older osvtool wrote the document to a file called "-"
                    // in the scratch directory: read that before running again.
                    string dashFile = System.IO.Path.Combine(scratch, "-");
                    if (File.Exists(dashFile))
                    {
                        ProbeResult fromDash = ProbeResult.Parse(ReadAllTextShared(dashFile), clipPath, out parseError);
                        if (fromDash != null)
                        {
                            return fromDash;
                        }
                    }
                    Log.Debug("osvtool probe: stdout was not a document (" + parseError + "); trying --json <file>");
                }
                else
                {
                    Log.Debug("osvtool probe --json - failed (" + run.Headline + "); trying --json <file>");
                }

                // ---- 2. --json <file> (every osvtool) ---------------------------------------------
                string jsonPath = System.IO.Path.Combine(scratch, "probe.json");
                ProcessOutcome fileRun = ProcessRunner.Run(ExePath, new[] { "probe", input, "--json", jsonPath }, scratch, ProbeTimeout, cancel);
                if (fileRun.Cancelled)
                {
                    error = "cancelled";
                    return null;
                }
                if (!fileRun.Succeeded || !File.Exists(jsonPath))
                {
                    error = "osvtool could not read the clip: " + fileRun.Headline;
                    return null;
                }
                ProbeResult fromFile = ProbeResult.Parse(ReadAllTextShared(jsonPath), clipPath, out string fileError);
                if (fromFile == null)
                {
                    error = fileError;
                }
                return fromFile;
            }
            catch (Exception ex)
            {
                error = "probing failed: " + ex.Message;
                Log.Error("osvtool probe of '" + clipPath + "' threw", ex);
                return null;
            }
            finally
            {
                DeleteScratch(scratch);
            }
        }

        // =====================================================================
        //  extract --audio
        // =====================================================================

        /// <summary>
        /// A clip's audio as a WAV in <paramref name="cache"/>: reused when a
        /// valid extraction exists, otherwise written by
        /// <c>osvtool extract --audio</c>.  When this osvtool writes only AAC,
        /// the result says so (<see cref="AudioExtraction.WavUnsupported"/>);
        /// call <see cref="ExtractAac"/> for the fallback.  Never throws.
        /// </summary>
        /// <param name="clipPath">The .OSV / .LRF.</param>
        /// <param name="cache">The audio cache.</param>
        /// <param name="durationSeconds">The clip's length, to size the timeout.</param>
        /// <param name="cancel">Cancels the run.</param>
        public AudioExtraction ExtractWav(string clipPath, AudioCache cache, double durationSeconds, CancellationToken cancel)
        {
            var result = new AudioExtraction();
            try
            {
                // ---- reuse ---------------------------------------------------------------
                if (cache.TryGetValid(clipPath, out string cached, out WavInfo cachedInfo))
                {
                    result.Path = cached;
                    result.IsWav = true;
                    result.FromCache = true;
                    result.Wav = cachedInfo;
                    return result;
                }
                string finalPath = cache.PathFor(clipPath, ".wav");
                if (finalPath == null)
                {
                    result.Error = "the clip cannot be read";
                    return result;
                }
                if (!AppPaths.EnsureDir(cache.Directory))
                {
                    result.Error = "cannot create the audio cache folder " + cache.Directory;
                    return result;
                }

                // ---- run ------------------------------------------------------------------------
                string partial = AudioCache.PartialPathFor(finalPath);
                AudioCache.TryDelete(partial);
                ProcessOutcome run = ProcessRunner.Run(
                    ExePath,
                    new[] { "extract", CommandLine.AsciiSafePath(clipPath), "--audio", CommandLine.AsciiSafePath(partial) },
                    cache.Directory, ExtractTimeout(durationSeconds), cancel);
                if (run.Cancelled)
                {
                    AudioCache.TryDelete(partial);
                    result.Cancelled = true;
                    result.Error = "cancelled";
                    return result;
                }
                if (!run.Succeeded)
                {
                    AudioCache.TryDelete(partial);
                    result.Error = "osvtool could not extract the audio: " + run.Headline;
                    return result;
                }

                // ---- trust, but verify ------------------------------------------------------------
                AudioFileKind kind = WavFile.Sniff(partial);
                if (kind == AudioFileKind.AdtsAac)
                {
                    // An osvtool from before WAV support: AAC in a .wav name.
                    AudioCache.TryDelete(partial);
                    result.WavUnsupported = true;
                    result.Error = "this osvtool writes AAC only";
                    return result;
                }
                WavInfo info = WavFile.TryRead(partial);
                if (info == null)
                {
                    AudioCache.TryDelete(partial);
                    result.Error = "osvtool's WAV is unreadable";
                    return result;
                }
                if (!AudioCache.Promote(partial, finalPath))
                {
                    result.Error = "cannot move the WAV into the audio cache";
                    return result;
                }
                result.Path = finalPath;
                result.IsWav = true;
                result.Wav = info;
                return result;
            }
            catch (Exception ex)
            {
                Log.Error("osvtool extract --audio of '" + clipPath + "' threw", ex);
                result.Error = "audio extraction failed: " + ex.Message;
                return result;
            }
        }

        /// <summary>
        /// The older osvtool's audio: the AAC track as an ADTS .aac in the
        /// cache.  Not sync-exact (the encoder's priming samples are not
        /// trimmed; ~21 ms early at 48 kHz), which is why the WAV path is
        /// always tried first.  Never throws.
        /// </summary>
        public AudioExtraction ExtractAac(string clipPath, AudioCache cache, double durationSeconds, CancellationToken cancel)
        {
            var result = new AudioExtraction();
            try
            {
                string finalPath = cache.PathFor(clipPath, ".aac");
                if (finalPath == null)
                {
                    result.Error = "the clip cannot be read";
                    return result;
                }
                if (SiblingFiles.Exists(finalPath) && WavFile.Sniff(finalPath) == AudioFileKind.AdtsAac)
                {
                    result.Path = finalPath;
                    result.FromCache = true;
                    return result;
                }
                if (!AppPaths.EnsureDir(cache.Directory))
                {
                    result.Error = "cannot create the audio cache folder " + cache.Directory;
                    return result;
                }
                string partial = AudioCache.PartialPathFor(finalPath);
                AudioCache.TryDelete(partial);
                ProcessOutcome run = ProcessRunner.Run(
                    ExePath,
                    new[] { "extract", CommandLine.AsciiSafePath(clipPath), "--audio", CommandLine.AsciiSafePath(partial) },
                    cache.Directory, ExtractTimeout(durationSeconds), cancel);
                if (run.Cancelled)
                {
                    AudioCache.TryDelete(partial);
                    result.Cancelled = true;
                    result.Error = "cancelled";
                    return result;
                }
                if (!run.Succeeded || WavFile.Sniff(partial) != AudioFileKind.AdtsAac)
                {
                    AudioCache.TryDelete(partial);
                    result.Error = "osvtool could not extract the audio: " + run.Headline;
                    return result;
                }
                if (!AudioCache.Promote(partial, finalPath))
                {
                    result.Error = "cannot move the audio into the cache";
                    return result;
                }
                result.Path = finalPath;
                return result;
            }
            catch (Exception ex)
            {
                Log.Error("osvtool extract --audio (AAC) of '" + clipPath + "' threw", ex);
                result.Error = "audio extraction failed: " + ex.Message;
                return result;
            }
        }

        /// <summary>
        /// Extraction timeout: a minute plus four times real time (decoding
        /// AAC is far faster than that; the margin is for a network drive).
        /// Capped at two hours.
        /// </summary>
        public static TimeSpan ExtractTimeout(double durationSeconds)
        {
            double seconds = 60.0 + 4.0 * (durationSeconds > 0 && !double.IsInfinity(durationSeconds) ? durationSeconds : 0.0);
            return TimeSpan.FromSeconds(Math.Min(seconds, 7200.0));
        }

        /// <summary>The first line of <c>osvtool --version</c>, or null.</summary>
        public string Version(CancellationToken cancel)
        {
            ProcessOutcome run = ProcessRunner.Run(ExePath, new[] { "--version" }, null, TimeSpan.FromSeconds(20), cancel);
            if (!run.Succeeded)
            {
                return null;
            }
            foreach (string line in run.StdOut.Split('\n'))
            {
                string t = line.Trim();
                if (t.Length > 0)
                {
                    return t;
                }
            }
            return null;
        }

        // ---- scratch directories ------------------------------------------------------

        private static string MakeScratchDir()
        {
            string dir = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "OpenOSV-vegas",
                                               Guid.NewGuid().ToString("N", CultureInfo.InvariantCulture));
            AppPaths.EnsureDir(dir);
            return dir;
        }

        private static void DeleteScratch(string dir)
        {
            try
            {
                if (Directory.Exists(dir))
                {
                    Directory.Delete(dir, true);
                }
            }
            catch (Exception)
            {
                // A virus scanner holding the file: the temp folder is cleaned eventually.
            }
        }

        private static string ReadAllTextShared(string path)
        {
            using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
            using (var reader = new StreamReader(fs, System.Text.Encoding.UTF8, true))
            {
                return reader.ReadToEnd();
            }
        }
    }
}
