// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// HygieneTests.cs - no local path may reach a shipped binary.
//
// A managed DLL records source and PDB paths unless the build maps them
// (Directory.Build.props: deterministic, embedded PDB, PathMap).  The release
// scan rejects any binary that names the machine it was built on; this test
// catches it at build-test time instead, on the staged extension.

using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace OpenOSV.Vegas.Tests
{
    public static class HygieneTests
    {
        [Test]
        public static void StagedBinariesNameNoLocalPath()
        {
            if (string.IsNullOrEmpty(TestContext.ScanDir))
            {
                throw new SkipException("no --scan-dir");
            }
            if (!Directory.Exists(TestContext.ScanDir))
            {
                Check.Fail("the staged folder does not exist: " + TestContext.ScanDir);
            }
            string[] dlls = Directory.GetFiles(TestContext.ScanDir, "*.dll");
            Check.True(dlls.Length >= 2, "OpenOSV.Vegas.dll and OpenOSV.Vegas.Core.dll are staged (found " + dlls.Length + ")");

            // ---- what must not appear: the given folders and this user's profile ------------
            var forbidden = new List<string>();
            foreach (string f in TestContext.Forbidden)
            {
                forbidden.Add(f.Replace('/', '\\').TrimEnd('\\'));
                forbidden.Add(f.Replace('\\', '/').TrimEnd('/'));
            }
            string profile = Environment.GetEnvironmentVariable("USERPROFILE");
            if (!string.IsNullOrEmpty(profile))
            {
                forbidden.Add(profile.TrimEnd('\\'));
            }

            foreach (string dll in dlls)
            {
                byte[] bytes = File.ReadAllBytes(dll);
                string ascii = Encoding.ASCII.GetString(bytes);
                string utf16 = Encoding.Unicode.GetString(bytes);
                string utf16Odd = bytes.Length > 1 ? Encoding.Unicode.GetString(bytes, 1, bytes.Length - 1) : string.Empty;
                foreach (string f in forbidden)
                {
                    if (f.Length < 4)
                    {
                        continue;
                    }
                    bool found = ascii.IndexOf(f, StringComparison.OrdinalIgnoreCase) >= 0 ||
                                 utf16.IndexOf(f, StringComparison.OrdinalIgnoreCase) >= 0 ||
                                 utf16Odd.IndexOf(f, StringComparison.OrdinalIgnoreCase) >= 0;
                    Check.False(found, Path.GetFileName(dll) + " contains the local path '" + f + "'");
                }
            }
        }
    }
}
