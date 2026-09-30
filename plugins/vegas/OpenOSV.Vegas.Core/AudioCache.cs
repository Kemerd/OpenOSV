// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// AudioCache.cs - where each clip's extracted audio lives, and when an
// existing extraction can be reused.

using System;
using System.Globalization;
using System.IO;
using System.Security.Cryptography;
using System.Text;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// The cache of extracted clip audio, <c>%LOCALAPPDATA%\OpenOSV\vegas-audio\</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// VEGAS cannot read an .OSV's audio track, so the extension has osvtool
    /// write it out as a 32-bit float WAV and puts that file on the timeline.
    /// The file is keyed by the clip's full path, size and modification time:
    /// importing the same clip again - into the same project or another -
    /// reuses it, and a clip that was re-copied or edited gets a new file
    /// instead of stale audio.
    /// </para>
    /// <para>
    /// The name keeps the clip's own name for people browsing the folder and
    /// adds a 16-hex-digit key: <c>CAM_20260904_0010_D-3f2a9c01b7d4e6a0.wav</c>.
    /// Only printable ASCII is kept in the name, so the path survives a tool
    /// that handles its arguments as narrow strings.
    /// </para>
    /// <para>
    /// Writes go to a <c>.partial.wav</c> beside the final name and are moved
    /// into place only after the header checks out, so a crash or a cancel
    /// never leaves a half file that a later import would trust.
    /// </para>
    /// </remarks>
    public sealed class AudioCache
    {
        /// <summary>Create a cache over <paramref name="directory"/>.</summary>
        public AudioCache(string directory)
        {
            Directory = string.IsNullOrWhiteSpace(directory) ? AppPaths.AudioCacheDir : directory;
        }

        /// <summary>The cache folder.</summary>
        public string Directory { get; }

        /// <summary>
        /// The cache key of a source file: SHA-1 over its lower-cased full path,
        /// size and last-write time (UTC ticks), first 16 hex digits.
        /// </summary>
        public static string KeyFor(string fullPath, long size, DateTime lastWriteUtc)
        {
            string material = (fullPath ?? string.Empty).ToLowerInvariant() + "|" +
                              size.ToString(CultureInfo.InvariantCulture) + "|" +
                              lastWriteUtc.ToUniversalTime().Ticks.ToString(CultureInfo.InvariantCulture);
            using (var sha = SHA1.Create())
            {
                byte[] hash = sha.ComputeHash(Encoding.UTF8.GetBytes(material));
                var sb = new StringBuilder(16);
                for (int i = 0; i < 8; ++i)
                {
                    sb.Append(hash[i].ToString("x2", CultureInfo.InvariantCulture));
                }
                return sb.ToString();
            }
        }

        /// <summary>
        /// The cache file for a source clip, or null when the source cannot be
        /// read (missing, no permission).  <paramref name="extension"/> is
        /// ".wav" (or ".aac" for the older osvtool fallback).
        /// </summary>
        public string PathFor(string sourcePath, string extension = ".wav")
        {
            try
            {
                var info = new FileInfo(sourcePath);
                if (!info.Exists)
                {
                    return null;
                }
                string key = KeyFor(info.FullName, info.Length, info.LastWriteTimeUtc);
                string stem = AsciiStem(Path.GetFileNameWithoutExtension(info.Name));
                string ext = string.IsNullOrEmpty(extension) ? ".wav" : (extension.StartsWith(".", StringComparison.Ordinal) ? extension : "." + extension);
                return Path.Combine(Directory, stem + "-" + key + ext);
            }
            catch (Exception)
            {
                return null;
            }
        }

        /// <summary>
        /// The reusable extraction of a source clip: the cached WAV when it
        /// exists and its header checks out.  A cached file that fails the
        /// check is deleted so the next extraction starts clean.
        /// </summary>
        public bool TryGetValid(string sourcePath, out string wavPath, out WavInfo info)
        {
            wavPath = PathFor(sourcePath);
            info = null;
            if (wavPath == null || !SiblingFiles.Exists(wavPath))
            {
                return false;
            }
            info = WavFile.TryRead(wavPath);
            if (info != null)
            {
                return true;
            }
            TryDelete(wavPath);
            wavPath = PathFor(sourcePath);
            return false;
        }

        /// <summary>The temporary name an extraction writes to before it is trusted.</summary>
        public static string PartialPathFor(string finalPath)
        {
            string dir = Path.GetDirectoryName(finalPath) ?? string.Empty;
            string stem = Path.GetFileNameWithoutExtension(finalPath);
            string ext = Path.GetExtension(finalPath);
            return Path.Combine(dir, stem + ".partial" + ext);
        }

        /// <summary>
        /// Move a finished partial file into place, replacing any older file.
        /// Returns false (and leaves the partial for the log) when it cannot.
        /// </summary>
        public static bool Promote(string partialPath, string finalPath)
        {
            try
            {
                if (File.Exists(finalPath))
                {
                    File.Delete(finalPath);
                }
                File.Move(partialPath, finalPath);
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("audio cache: cannot move '" + partialPath + "' into place", ex);
                return false;
            }
        }

        /// <summary>Delete a file, ignoring every failure.</summary>
        public static void TryDelete(string path)
        {
            try
            {
                if (!string.IsNullOrEmpty(path) && File.Exists(path))
                {
                    File.Delete(path);
                }
            }
            catch (Exception)
            {
                // In use or gone: nothing to do.
            }
        }

        /// <summary>Total bytes in the cache folder (0 when it does not exist).</summary>
        public long SizeBytes()
        {
            long total = 0;
            try
            {
                if (!System.IO.Directory.Exists(Directory))
                {
                    return 0;
                }
                foreach (string f in System.IO.Directory.EnumerateFiles(Directory))
                {
                    try
                    {
                        total += new FileInfo(f).Length;
                    }
                    catch (Exception)
                    {
                        // Deleted while counting.
                    }
                }
            }
            catch (Exception)
            {
                // Unreadable folder: report what we counted.
            }
            return total;
        }

        /// <summary>A file-name stem reduced to printable ASCII (at most 80 characters).</summary>
        internal static string AsciiStem(string stem)
        {
            if (string.IsNullOrEmpty(stem))
            {
                return "clip";
            }
            var sb = new StringBuilder(stem.Length);
            foreach (char c in stem)
            {
                bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
                sb.Append(ok ? c : '_');
                if (sb.Length >= 80)
                {
                    break;
                }
            }
            string result = sb.ToString().Trim('.', ' ');
            return result.Length == 0 ? "clip" : result;
        }
    }
}
