// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Program.cs - the test runner's entry point.
//
//   OpenOSV.Vegas.Tests.exe [--source-root <repo>] [--osvtool <osvtool.exe>]
//                           [--sample <clip.OSV>] [--filter <text>]
//
// ctest passes all three paths.  Without --source-root the runner looks for
// the repository above its own folder and the current directory; without
// --osvtool / --sample the live tests skip (CI has no footage).  Exit code =
// number of failed tests.

using System;
using System.IO;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.Tests
{
    public static class Program
    {
        public static int Main(string[] args)
        {
            string filter = null;
            try
            {
                // ---- arguments ------------------------------------------------------------
                for (int i = 0; i < args.Length; ++i)
                {
                    string a = args[i];
                    string next = i + 1 < args.Length ? args[i + 1] : null;
                    switch (a)
                    {
                        case "--source-root": TestContext.SourceRoot = next; ++i; break;
                        case "--osvtool": TestContext.OsvTool = next; ++i; break;
                        case "--sample": TestContext.SampleOsv = next; ++i; break;
                        case "--filter": filter = next; ++i; break;
                        case "--scan-dir": TestContext.ScanDir = next; ++i; break;
                        case "--scriptportal": TestContext.ScriptPortal = next; ++i; break;
                        case "--forbid":
                            if (!string.IsNullOrEmpty(next))
                            {
                                TestContext.Forbidden.Add(next);
                            }
                            ++i;
                            break;
                        default:
                            Console.Error.WriteLine("unknown argument: " + a);
                            return 100;
                    }
                }
                if (string.IsNullOrEmpty(TestContext.SourceRoot))
                {
                    TestContext.SourceRoot = FindSourceRoot(AppDomain.CurrentDomain.BaseDirectory) ??
                                             FindSourceRoot(Environment.CurrentDirectory);
                }
                if (string.IsNullOrEmpty(TestContext.SampleOsv))
                {
                    TestContext.SampleOsv = Environment.GetEnvironmentVariable("OSV_SAMPLE_FILE");
                }

                // ---- isolation: nothing a test writes reaches the user's real files ------------
                TestContext.Scratch = Path.Combine(Path.GetTempPath(), "openosv-vegas-tests-" + Guid.NewGuid().ToString("N").Substring(0, 8));
                Directory.CreateDirectory(TestContext.Scratch);
                AppPaths.OverrideRoots(Path.Combine(TestContext.Scratch, "local"), Path.Combine(TestContext.Scratch, "roaming"));

                Console.WriteLine("OpenOSV.Vegas.Tests");
                Console.WriteLine("  source root : " + (TestContext.SourceRoot ?? "(not found)"));
                Console.WriteLine("  osvtool     : " + (TestContext.OsvTool ?? "(none)"));
                Console.WriteLine("  sample      : " + (TestContext.SampleOsv ?? "(none)"));
                Console.WriteLine("  scratch     : " + TestContext.Scratch);
                Console.WriteLine();

                int failed = Runner.Run(filter);
                return failed;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("the test runner itself failed: " + ex);
                return 101;
            }
            finally
            {
                try
                {
                    if (TestContext.Scratch != null && Directory.Exists(TestContext.Scratch))
                    {
                        Directory.Delete(TestContext.Scratch, true);
                    }
                }
                catch (Exception)
                {
                    // A file still open by a killed process: the temp folder cleans it later.
                }
            }
        }

        /// <summary>The first folder at or above <paramref name="start"/> holding plugins/ofx/OfxSource.h.</summary>
        private static string FindSourceRoot(string start)
        {
            try
            {
                var dir = new DirectoryInfo(start);
                while (dir != null)
                {
                    if (File.Exists(Path.Combine(dir.FullName, "plugins", "ofx", "OfxSource.h")))
                    {
                        return dir.FullName;
                    }
                    dir = dir.Parent;
                }
            }
            catch (Exception)
            {
                // No root: the header tests skip.
            }
            return null;
        }
    }
}
