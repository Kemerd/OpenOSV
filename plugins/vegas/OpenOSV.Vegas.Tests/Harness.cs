// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Harness.cs - the smallest test framework that does the job: a [Test]
// attribute, assertions that throw, and a runner that reports and exits with
// the number of failures.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Reflection;

namespace OpenOSV.Vegas.Tests
{
    /// <summary>Marks a public static void method as a test.</summary>
    [AttributeUsage(AttributeTargets.Method)]
    public sealed class TestAttribute : Attribute
    {
    }

    /// <summary>Thrown by a test that cannot run here (a missing fixture): reported, not failed.</summary>
    public sealed class SkipException : Exception
    {
        public SkipException(string reason) : base(reason)
        {
        }
    }

    /// <summary>A failed assertion.</summary>
    public sealed class AssertionException : Exception
    {
        public AssertionException(string message) : base(message)
        {
        }
    }

    /// <summary>Assertions.  Each throws <see cref="AssertionException"/> with a readable message.</summary>
    public static class Check
    {
        public static void True(bool condition, string what)
        {
            if (!condition)
            {
                throw new AssertionException("expected true: " + what);
            }
        }

        public static void False(bool condition, string what)
        {
            if (condition)
            {
                throw new AssertionException("expected false: " + what);
            }
        }

        public static void Equal<T>(T expected, T actual, string what)
        {
            if (!EqualityComparer<T>.Default.Equals(expected, actual))
            {
                throw new AssertionException(what + ": expected <" + Show(expected) + "> but got <" + Show(actual) + ">");
            }
        }

        public static void Near(double expected, double actual, double tolerance, string what)
        {
            if (double.IsNaN(actual) || Math.Abs(expected - actual) > tolerance)
            {
                throw new AssertionException(what + ": expected " + expected.ToString("R", CultureInfo.InvariantCulture) +
                                             " +/- " + tolerance.ToString("R", CultureInfo.InvariantCulture) + " but got " +
                                             actual.ToString("R", CultureInfo.InvariantCulture));
            }
        }

        public static void NotNull(object value, string what)
        {
            if (value == null)
            {
                throw new AssertionException("expected a value: " + what);
            }
        }

        public static void Null(object value, string what)
        {
            if (value != null)
            {
                throw new AssertionException("expected null: " + what + " (got " + Show(value) + ")");
            }
        }

        public static void Contains(string haystack, string needle, string what)
        {
            if (haystack == null || needle == null || haystack.IndexOf(needle, StringComparison.Ordinal) < 0)
            {
                throw new AssertionException(what + ": expected '" + haystack + "' to contain '" + needle + "'");
            }
        }

        public static void Fail(string what) => throw new AssertionException(what);

        private static string Show(object o) => o == null ? "null" : Convert.ToString(o, CultureInfo.InvariantCulture);
    }

    /// <summary>What every test can read: paths given on the command line.</summary>
    public static class TestContext
    {
        /// <summary>The repository root (for the C++ headers), or null.</summary>
        public static string SourceRoot { get; set; }

        /// <summary>osvtool.exe for the live tests, or null.</summary>
        public static string OsvTool { get; set; }

        /// <summary>The sample .OSV for the live tests, or null.</summary>
        public static string SampleOsv { get; set; }

        /// <summary>A scratch folder, deleted at the end of the run.</summary>
        public static string Scratch { get; set; }

        /// <summary>ScriptPortal.Vegas.dll to compile the smoke-test script against, or null.</summary>
        public static string ScriptPortal { get; set; }

        /// <summary>A folder of built binaries to scan for local paths (vegas.hygiene), or null.</summary>
        public static string ScanDir { get; set; }

        /// <summary>Paths that must not appear inside a scanned binary.</summary>
        public static List<string> Forbidden { get; } = new List<string>();

        /// <summary>The Fixtures folder beside the runner.</summary>
        public static string Fixtures => Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "Fixtures");

        /// <summary>A fixture's text.</summary>
        public static string Fixture(string name)
        {
            string path = Path.Combine(Fixtures, name);
            if (!File.Exists(path))
            {
                throw new AssertionException("fixture missing: " + path);
            }
            return File.ReadAllText(path);
        }

        /// <summary>A new empty folder inside the scratch folder.</summary>
        public static string NewDir(string name)
        {
            string dir = Path.Combine(Scratch, name + "-" + Guid.NewGuid().ToString("N").Substring(0, 8));
            Directory.CreateDirectory(dir);
            return dir;
        }

        /// <summary>A file under the source root; skips the test when the root is unknown.</summary>
        public static string SourceFile(string relative)
        {
            if (string.IsNullOrEmpty(SourceRoot))
            {
                throw new SkipException("no --source-root");
            }
            string path = Path.Combine(SourceRoot, relative.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(path))
            {
                throw new AssertionException("source file missing: " + path);
            }
            return path;
        }
    }

    /// <summary>Finds every [Test] method, runs it, and reports.</summary>
    public static class Runner
    {
        public static int Run(string filter)
        {
            var tests = typeof(Runner).Assembly.GetTypes()
                .SelectMany(t => t.GetMethods(BindingFlags.Public | BindingFlags.Static))
                .Where(m => m.GetCustomAttributes(typeof(TestAttribute), false).Length > 0)
                .OrderBy(m => m.DeclaringType.Name, StringComparer.Ordinal)
                .ThenBy(m => m.Name, StringComparer.Ordinal)
                .ToList();
            int passed = 0, failed = 0, skipped = 0;
            var watch = Stopwatch.StartNew();
            foreach (MethodInfo m in tests)
            {
                string name = m.DeclaringType.Name + "." + m.Name;
                if (!string.IsNullOrEmpty(filter) && name.IndexOf(filter, StringComparison.OrdinalIgnoreCase) < 0)
                {
                    continue;
                }
                var one = Stopwatch.StartNew();
                try
                {
                    m.Invoke(null, null);
                    ++passed;
                    Console.WriteLine("  pass  " + name + " (" + one.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms)");
                }
                catch (TargetInvocationException tie) when (tie.InnerException is SkipException skip)
                {
                    ++skipped;
                    Console.WriteLine("  skip  " + name + ": " + skip.Message);
                }
                catch (TargetInvocationException tie)
                {
                    ++failed;
                    Exception inner = tie.InnerException ?? tie;
                    Console.WriteLine("  FAIL  " + name + ": " + inner.Message);
                    if (!(inner is AssertionException))
                    {
                        Console.WriteLine(inner.ToString());
                    }
                }
            }
            Console.WriteLine();
            Console.WriteLine(passed.ToString(CultureInfo.InvariantCulture) + " passed, " +
                              failed.ToString(CultureInfo.InvariantCulture) + " failed, " +
                              skipped.ToString(CultureInfo.InvariantCulture) + " skipped in " +
                              watch.ElapsedMilliseconds.ToString(CultureInfo.InvariantCulture) + " ms");
            return failed;
        }
    }
}
