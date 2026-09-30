// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// SiblingFiles.cs - which files are OSV clips, and the .OSV <-> .LRF pair.

using System;
using System.Collections.Generic;
using System.IO;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// File-name rules for DJI Osmo 360 clips.
    /// </summary>
    /// <remarks>
    /// The camera writes every recording twice: the full-quality <c>.OSV</c>
    /// and a small <c>.LRF</c> proxy beside it with the same name
    /// (<c>CAM_..._D.OSV</c> / <c>CAM_..._D.LRF</c>).  These rules are the
    /// importer's own (<c>ImporterInstance::proxyOriginalFor</c>): same folder,
    /// same stem, the other extension in either case.  The extension check is
    /// case-insensitive and exact, so <c>trip.osv.mp4</c> is not a clip.
    /// </remarks>
    public static class SiblingFiles
    {
        /// <summary>True when the path ends in <c>.osv</c> (any case).</summary>
        public static bool IsOsv(string path) => HasExtension(path, ".osv");

        /// <summary>True when the path ends in <c>.lrf</c> (any case).</summary>
        public static bool IsLrf(string path) => HasExtension(path, ".lrf");

        /// <summary>True for either kind of clip.</summary>
        public static bool IsSupported(string path) => IsOsv(path) || IsLrf(path);

        /// <summary>
        /// The existing sibling of a clip: the <c>.LRF</c> of an <c>.OSV</c>,
        /// or the <c>.OSV</c> of an <c>.LRF</c>.  Null when there is none, when
        /// the path is neither kind, or when the path is unusable.
        /// </summary>
        /// <param name="path">A clip path.</param>
        public static string SiblingOf(string path)
        {
            if (IsOsv(path))
            {
                return FirstExisting(path, ".LRF", ".lrf");
            }
            if (IsLrf(path))
            {
                return FirstExisting(path, ".OSV", ".osv");
            }
            return null;
        }

        /// <summary>The <c>.LRF</c> beside an <c>.OSV</c>, or null.</summary>
        public static string ProxyOf(string osvPath) => IsOsv(osvPath) ? FirstExisting(osvPath, ".LRF", ".lrf") : null;

        /// <summary>The <c>.OSV</c> beside an <c>.LRF</c>, or null.</summary>
        public static string OriginalOf(string lrfPath) => IsLrf(lrfPath) ? FirstExisting(lrfPath, ".OSV", ".osv") : null;

        /// <summary>
        /// Keep only the clip files of a list (drag and drop, a file dialog),
        /// de-duplicated by full path, in the order given.
        /// </summary>
        public static List<string> FilterClips(IEnumerable<string> paths)
        {
            var result = new List<string>();
            var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (paths == null)
            {
                return result;
            }
            foreach (string p in paths)
            {
                if (string.IsNullOrWhiteSpace(p) || !IsSupported(p))
                {
                    continue;
                }
                string full = NormalizeOrSelf(p);
                if (seen.Add(full))
                {
                    result.Add(full);
                }
            }
            return result;
        }

        /// <summary><see cref="Path.GetFullPath"/>, or the input when it cannot be normalised.</summary>
        public static string NormalizeOrSelf(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return path ?? string.Empty;
            }
            try
            {
                return Path.GetFullPath(path.Trim().Trim('"'));
            }
            catch (Exception)
            {
                return path;
            }
        }

        /// <summary>True when the file exists (never throws).</summary>
        public static bool Exists(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return false;
            }
            try
            {
                return File.Exists(path);
            }
            catch (Exception)
            {
                return false;
            }
        }

        private static bool HasExtension(string path, string ext)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return false;
            }
            string trimmed = path.Trim().TrimEnd('"');
            return trimmed.EndsWith(ext, StringComparison.OrdinalIgnoreCase) && trimmed.Length > ext.Length &&
                   trimmed[trimmed.Length - ext.Length - 1] != '\\' && trimmed[trimmed.Length - ext.Length - 1] != '/';
        }

        private static string FirstExisting(string path, params string[] extensions)
        {
            try
            {
                foreach (string ext in extensions)
                {
                    string candidate = Path.ChangeExtension(path, ext);
                    if (File.Exists(candidate))
                    {
                        // On a case-insensitive volume both spellings "exist";
                        // report the file's real name as the directory lists it.
                        return RealCase(candidate);
                    }
                }
            }
            catch (Exception)
            {
                // A path the file system cannot handle has no sibling.
            }
            return null;
        }

        /// <summary>The path with its file name as the directory spells it.</summary>
        private static string RealCase(string path)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                string name = Path.GetFileName(path);
                if (string.IsNullOrEmpty(dir) || string.IsNullOrEmpty(name))
                {
                    return path;
                }
                foreach (string entry in Directory.EnumerateFiles(dir, name))
                {
                    return entry;
                }
            }
            catch (Exception)
            {
                // Keep the spelling we had.
            }
            return path;
        }
    }
}
