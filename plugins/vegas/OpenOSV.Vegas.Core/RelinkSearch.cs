// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// RelinkSearch.cs - find moved clips by file name under a folder.

using System;
using System.Collections.Generic;
using System.IO;
using System.Threading;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// Walks a folder tree once and collects every file whose name is one of
    /// the names being looked for.
    /// </summary>
    /// <remarks>
    /// One walk for any number of missing clips: a card reader dumped into
    /// "Footage\2026\" is searched once, not once per clip.  The walk is
    /// iterative (no recursion depth to overflow), skips reparse points (no
    /// junction loops), survives folders it may not read, and stops at a
    /// depth and entry budget so pointing it at C:\ cannot hang VEGAS.
    /// </remarks>
    public static class RelinkSearch
    {
        /// <summary>Deepest folder level searched below the root.</summary>
        public const int DefaultMaxDepth = 24;

        /// <summary>Most directory entries looked at before giving up.</summary>
        public const int DefaultMaxEntries = 2_000_000;

        /// <summary>
        /// Every file under <paramref name="root"/> whose name (any case) is in
        /// <paramref name="fileNames"/>: name -> full paths, in walk order.
        /// </summary>
        /// <param name="root">The folder to search.</param>
        /// <param name="fileNames">Bare file names ("CAM_0001.OSV").</param>
        /// <param name="cancel">Stops the walk.</param>
        /// <param name="progress">Called now and then with the folders searched so far.</param>
        /// <param name="maxDepth">Folder depth limit.</param>
        /// <param name="maxEntries">Entry budget.</param>
        public static Dictionary<string, List<string>> FindByName(string root, IEnumerable<string> fileNames, CancellationToken cancel,
                                                                  Action<int> progress = null, int maxDepth = DefaultMaxDepth,
                                                                  int maxEntries = DefaultMaxEntries)
        {
            var found = new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase);
            var wanted = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            if (fileNames != null)
            {
                foreach (string n in fileNames)
                {
                    if (!string.IsNullOrWhiteSpace(n))
                    {
                        wanted.Add(n.Trim());
                    }
                }
            }
            if (wanted.Count == 0 || string.IsNullOrWhiteSpace(root))
            {
                return found;
            }
            bool rootExists;
            try
            {
                rootExists = Directory.Exists(root);
            }
            catch (Exception)
            {
                rootExists = false;
            }
            if (!rootExists)
            {
                return found;
            }

            // ---- iterative depth-first walk ---------------------------------------------
            var stack = new Stack<KeyValuePair<string, int>>();
            stack.Push(new KeyValuePair<string, int>(root, 0));
            int entries = 0;
            int folders = 0;
            while (stack.Count > 0 && entries < maxEntries)
            {
                if (cancel.IsCancellationRequested)
                {
                    break;
                }
                KeyValuePair<string, int> item = stack.Pop();
                ++folders;
                if (progress != null && folders % 64 == 0)
                {
                    progress(folders);
                }

                // ---- files of this folder -------------------------------------------------------
                try
                {
                    foreach (string file in Directory.EnumerateFiles(item.Key))
                    {
                        if (++entries >= maxEntries)
                        {
                            break;
                        }
                        string name = Path.GetFileName(file);
                        if (wanted.Contains(name))
                        {
                            if (!found.TryGetValue(name, out List<string> list))
                            {
                                list = new List<string>();
                                found[name] = list;
                            }
                            list.Add(file);
                        }
                    }
                }
                catch (Exception)
                {
                    // No permission, a vanished folder, a path too long: skip it.
                    continue;
                }

                // ---- sub-folders ------------------------------------------------------------------
                if (item.Value >= maxDepth)
                {
                    continue;
                }
                try
                {
                    var subdirs = new List<string>();
                    foreach (string sub in Directory.EnumerateDirectories(item.Key))
                    {
                        if (++entries >= maxEntries)
                        {
                            break;
                        }
                        try
                        {
                            // Junctions and symlinks can loop; the recycle bin and
                            // the volume's own bookkeeping never hold footage.  (A
                            // folder with a custom icon carries the System bit, so
                            // that bit alone is not a reason to skip.)
                            var attributes = File.GetAttributes(sub);
                            if ((attributes & FileAttributes.ReparsePoint) != 0)
                            {
                                continue;
                            }
                            string subName = Path.GetFileName(sub);
                            if (string.Equals(subName, "$RECYCLE.BIN", StringComparison.OrdinalIgnoreCase) ||
                                string.Equals(subName, "System Volume Information", StringComparison.OrdinalIgnoreCase))
                            {
                                continue;
                            }
                        }
                        catch (Exception)
                        {
                            continue;
                        }
                        subdirs.Add(sub);
                    }
                    // Push in reverse so folders are visited alphabetically.
                    for (int i = subdirs.Count - 1; i >= 0; --i)
                    {
                        stack.Push(new KeyValuePair<string, int>(subdirs[i], item.Value + 1));
                    }
                }
                catch (Exception)
                {
                    // Unreadable sub-folder list: the files above still count.
                }
            }
            return found;
        }

        /// <summary>
        /// The best candidate for a missing file: one with the recorded size
        /// when the size is known, else one whose parent folder has the same
        /// name as the missing file's, else the first found.  Null for none.
        /// </summary>
        public static string PickBest(string missingPath, IList<string> candidates, long expectedSize)
        {
            if (candidates == null || candidates.Count == 0)
            {
                return null;
            }
            // ---- 1. exact size ------------------------------------------------------------
            if (expectedSize > 0)
            {
                foreach (string c in candidates)
                {
                    try
                    {
                        if (new FileInfo(c).Length == expectedSize)
                        {
                            return c;
                        }
                    }
                    catch (Exception)
                    {
                        // Unreadable candidate: not this one.
                    }
                }
            }
            // ---- 2. same parent folder name ------------------------------------------------------
            string parent = ParentName(missingPath);
            if (!string.IsNullOrEmpty(parent))
            {
                foreach (string c in candidates)
                {
                    if (string.Equals(ParentName(c), parent, StringComparison.OrdinalIgnoreCase))
                    {
                        return c;
                    }
                }
            }
            // ---- 3. the first -----------------------------------------------------------------------
            return candidates[0];
        }

        private static string ParentName(string path)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                return string.IsNullOrEmpty(dir) ? null : Path.GetFileName(dir);
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
