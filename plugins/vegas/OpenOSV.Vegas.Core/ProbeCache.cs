// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProbeCache.cs - remember what osvtool said about a file for the session.

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Threading;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// Probe results by file identity (full path, size, modification time), so
    /// switching fifty clips to proxies and back probes each file once.  A file
    /// that changes on disk gets a new key and is probed again.  Thread-safe.
    /// </summary>
    public sealed class ProbeCache
    {
        /// <summary>Most entries kept (oldest dropped first).</summary>
        public const int Capacity = 512;

        private readonly object _gate = new object();
        private readonly Dictionary<string, ProbeResult> _byKey = new Dictionary<string, ProbeResult>(StringComparer.OrdinalIgnoreCase);
        private readonly Queue<string> _order = new Queue<string>();

        /// <summary>
        /// The probe of <paramref name="path"/>: cached when the file is
        /// unchanged, else run through <paramref name="tool"/>.  Null with
        /// <paramref name="error"/> on failure (failures are not cached).
        /// </summary>
        public ProbeResult Get(OsvTool tool, string path, CancellationToken cancel, out string error)
        {
            error = null;
            string key = KeyOf(path);
            if (key == null)
            {
                error = "the file is missing: " + path;
                return null;
            }
            lock (_gate)
            {
                if (_byKey.TryGetValue(key, out ProbeResult hit))
                {
                    return hit;
                }
            }
            if (tool == null)
            {
                error = "osvtool is not available";
                return null;
            }
            ProbeResult fresh = tool.Probe(path, cancel, out error);
            if (fresh == null)
            {
                return null;
            }
            lock (_gate)
            {
                if (!_byKey.ContainsKey(key))
                {
                    _byKey[key] = fresh;
                    _order.Enqueue(key);
                    while (_order.Count > Capacity)
                    {
                        _byKey.Remove(_order.Dequeue());
                    }
                }
            }
            return fresh;
        }

        /// <summary>Forget everything.</summary>
        public void Clear()
        {
            lock (_gate)
            {
                _byKey.Clear();
                _order.Clear();
            }
        }

        /// <summary>path|size|mtime, or null when the file cannot be read.</summary>
        private static string KeyOf(string path)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists)
                {
                    return null;
                }
                return info.FullName + "|" + info.Length.ToString(CultureInfo.InvariantCulture) + "|" +
                       info.LastWriteTimeUtc.Ticks.ToString(CultureInfo.InvariantCulture);
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
