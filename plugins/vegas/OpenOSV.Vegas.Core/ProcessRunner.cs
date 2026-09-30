// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProcessRunner.cs - run a console tool invisibly, with a timeout, a cancel
// button's token and both output streams captured.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace OpenOSV.Vegas.Core
{
    /// <summary>What happened when a tool ran.</summary>
    public sealed class ProcessOutcome
    {
        /// <summary>The process exit code (-1 when it never ran or was killed).</summary>
        public int ExitCode { get; internal set; } = -1;

        /// <summary>Everything the tool wrote to stdout (UTF-8).</summary>
        public string StdOut { get; internal set; } = string.Empty;

        /// <summary>Everything the tool wrote to stderr (UTF-8).</summary>
        public string StdErr { get; internal set; } = string.Empty;

        /// <summary>True when the timeout killed it.</summary>
        public bool TimedOut { get; internal set; }

        /// <summary>True when the cancellation token killed it.</summary>
        public bool Cancelled { get; internal set; }

        /// <summary>Why it could not be started at all (null when it started).</summary>
        public string LaunchError { get; internal set; }

        /// <summary>Wall time from start to exit.</summary>
        public TimeSpan Elapsed { get; internal set; }

        /// <summary>True when it started, finished by itself and returned 0.</summary>
        public bool Succeeded => LaunchError == null && !TimedOut && !Cancelled && ExitCode == 0;

        /// <summary>
        /// The most useful single line for a message: the last "error:" line of
        /// stderr, else its last non-empty line, else the launch / timeout
        /// reason, else the exit code.
        /// </summary>
        public string Headline
        {
            get
            {
                if (LaunchError != null)
                {
                    return LaunchError;
                }
                if (Cancelled)
                {
                    return "cancelled";
                }
                if (TimedOut)
                {
                    return "took too long and was stopped";
                }
                // The last "error:" line wins; any other last line is second best.
                string lastError = null;
                string lastLine = null;
                foreach (string raw in (StdErr ?? string.Empty).Split('\n'))
                {
                    string line = raw.Trim();
                    if (line.Length == 0)
                    {
                        continue;
                    }
                    lastLine = line;
                    if (line.StartsWith("error:", StringComparison.OrdinalIgnoreCase))
                    {
                        lastError = line.Substring(6).Trim();
                    }
                }
                if (!string.IsNullOrEmpty(lastError))
                {
                    return lastError;
                }
                if (!string.IsNullOrEmpty(lastLine))
                {
                    return lastLine;
                }
                return "exit code " + ExitCode.ToString(CultureInfo.InvariantCulture);
            }
        }
    }

    /// <summary>Runs console tools for the extension.</summary>
    public static class ProcessRunner
    {
        /// <summary>Largest output kept per stream, in characters (the rest is dropped).</summary>
        public const int MaxCapturedChars = 32 * 1024 * 1024;

        /// <summary>
        /// Run <paramref name="exe"/> with <paramref name="args"/> and wait for
        /// it.  No console window appears, stdin is closed at once (a tool that
        /// waits for input fails instead of hanging), and both output streams
        /// are read concurrently so a chatty tool can never block on a full
        /// pipe.  Never throws.
        /// </summary>
        /// <param name="exe">Full path of the executable.</param>
        /// <param name="args">Arguments, each quoted per the MSVC rules.</param>
        /// <param name="workingDirectory">Working directory (null = the exe's folder).</param>
        /// <param name="timeout">Kill the tool after this long.</param>
        /// <param name="cancel">Kill the tool when this is cancelled.</param>
        public static ProcessOutcome Run(string exe, IEnumerable<string> args, string workingDirectory, TimeSpan timeout,
                                         CancellationToken cancel)
        {
            var outcome = new ProcessOutcome();
            var watch = Stopwatch.StartNew();
            if (string.IsNullOrWhiteSpace(exe) || !File.Exists(exe))
            {
                outcome.LaunchError = "the tool is missing (" + (exe ?? "no path") + ")";
                return outcome;
            }
            if (cancel.IsCancellationRequested)
            {
                outcome.Cancelled = true;
                return outcome;
            }

            // ---- the start info: invisible, redirected, UTF-8 ------------------------
            var psi = new ProcessStartInfo
            {
                FileName = exe,
                Arguments = CommandLine.Join(args),
                UseShellExecute = false,
                CreateNoWindow = true,
                WindowStyle = ProcessWindowStyle.Hidden,
                RedirectStandardInput = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = new UTF8Encoding(false),
                StandardErrorEncoding = new UTF8Encoding(false),
                WorkingDirectory = SafeWorkingDirectory(workingDirectory, exe),
            };

            Process process = null;
            KillOnCloseJob job = null;
            try
            {
                process = new Process { StartInfo = psi };
                try
                {
                    if (!process.Start())
                    {
                        outcome.LaunchError = "Windows refused to start " + SafeName(exe);
                        return outcome;
                    }
                }
                catch (Exception ex)
                {
                    outcome.LaunchError = "cannot start " + SafeName(exe) + ": " + ex.Message;
                    return outcome;
                }

                // ---- a kill-on-close job: the tool (and anything it starts)
                // dies with this run - or with VEGAS, if VEGAS goes first -
                // instead of decoding on as an orphan.
                job = KillOnCloseJob.TryCreateFor(process);

                // ---- stdin closed, both outputs drained in the background ---------------
                try
                {
                    process.StandardInput.Close();
                }
                catch (Exception)
                {
                    // A tool that already exited has no stdin to close.
                }
                Task<string> stdout = ReadCapped(process.StandardOutput);
                Task<string> stderr = ReadCapped(process.StandardError);

                // ---- wait: exit, timeout or cancel, polled every 50 ms ------------------------
                TimeSpan limit = timeout <= TimeSpan.Zero ? TimeSpan.FromMinutes(10) : timeout;
                while (true)
                {
                    if (process.WaitForExit(50))
                    {
                        break;
                    }
                    if (cancel.IsCancellationRequested)
                    {
                        outcome.Cancelled = true;
                        Kill(process, job);
                        break;
                    }
                    if (watch.Elapsed > limit)
                    {
                        outcome.TimedOut = true;
                        Kill(process, job);
                        break;
                    }
                }

                // ---- collect ---------------------------------------------------------------------
                try
                {
                    process.WaitForExit(5000);
                }
                catch (Exception)
                {
                    // Already gone.
                }
                // A tool that exited by itself has closed its pipes; a killed
                // one may have left a child holding them, so its streams get
                // one second, not five.
                bool killed = outcome.Cancelled || outcome.TimedOut;
                int streamWait = killed ? 1000 : 5000;
                outcome.StdOut = WaitText(stdout, streamWait);
                outcome.StdErr = WaitText(stderr, streamWait);
                try
                {
                    outcome.ExitCode = process.HasExited ? process.ExitCode : -1;
                }
                catch (Exception)
                {
                    outcome.ExitCode = -1;
                }
            }
            catch (Exception ex)
            {
                outcome.LaunchError = outcome.LaunchError ?? ("running " + SafeName(exe) + " failed: " + ex.Message);
            }
            finally
            {
                // Closing the job kills whatever is still in it.
                job?.Dispose();
                try
                {
                    process?.Dispose();
                }
                catch (Exception)
                {
                    // Nothing left to release.
                }
                outcome.Elapsed = watch.Elapsed;
            }
            return outcome;
        }

        /// <summary>Read a stream to its end, keeping at most <see cref="MaxCapturedChars"/>.</summary>
        private static Task<string> ReadCapped(StreamReader reader)
        {
            return Task.Run(() =>
            {
                var sb = new StringBuilder();
                var buffer = new char[16384];
                try
                {
                    int n;
                    while ((n = reader.Read(buffer, 0, buffer.Length)) > 0)
                    {
                        // Keep draining past the cap so the tool never blocks,
                        // but stop storing.
                        if (sb.Length < MaxCapturedChars)
                        {
                            sb.Append(buffer, 0, Math.Min(n, MaxCapturedChars - sb.Length));
                        }
                    }
                }
                catch (Exception)
                {
                    // A killed process closes its pipes abruptly.
                }
                return sb.ToString();
            });
        }

        private static string WaitText(Task<string> task, int milliseconds)
        {
            try
            {
                return task.Wait(milliseconds) ? (task.Result ?? string.Empty) : string.Empty;
            }
            catch (Exception)
            {
                return string.Empty;
            }
        }

        /// <summary>Stop the tool: its whole job when there is one, else the process.</summary>
        private static void Kill(Process process, KillOnCloseJob job)
        {
            job?.Terminate();
            try
            {
                if (!process.HasExited)
                {
                    process.Kill();
                }
            }
            catch (Exception)
            {
                // It exited between the check and the kill.
            }
        }

        private static string SafeWorkingDirectory(string requested, string exe)
        {
            try
            {
                if (!string.IsNullOrWhiteSpace(requested) && Directory.Exists(requested))
                {
                    return requested;
                }
                string dir = Path.GetDirectoryName(exe);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                {
                    return dir;
                }
            }
            catch (Exception)
            {
                // Fall back to the temp folder.
            }
            return Path.GetTempPath();
        }

        private static string SafeName(string exe)
        {
            try
            {
                return Path.GetFileName(exe);
            }
            catch (Exception)
            {
                return exe;
            }
        }
    }

    /// <summary>Command-line text for Windows programs.</summary>
    public static class CommandLine
    {
        /// <summary>Join arguments into one command line, quoting each as needed.</summary>
        public static string Join(IEnumerable<string> args)
        {
            if (args == null)
            {
                return string.Empty;
            }
            var sb = new StringBuilder();
            foreach (string a in args)
            {
                if (a == null)
                {
                    continue;
                }
                if (sb.Length > 0)
                {
                    sb.Append(' ');
                }
                sb.Append(Quote(a));
            }
            return sb.ToString();
        }

        /// <summary>
        /// Quote one argument so the MSVC runtime's parser (and
        /// CommandLineToArgvW, which osvtool's wmain receives) gives it back
        /// unchanged: backslashes are literal except before a quote, where each
        /// must be doubled, and a trailing run of backslashes is doubled before
        /// the closing quote - the case that breaks <c>"C:\folder\"</c>.
        /// </summary>
        public static string Quote(string arg)
        {
            if (arg == null)
            {
                return "\"\"";
            }
            if (arg.Length > 0 && arg.IndexOfAny(new[] { ' ', '\t', '\n', '\v', '"' }) < 0)
            {
                return arg;
            }
            var sb = new StringBuilder(arg.Length + 2);
            sb.Append('"');
            int backslashes = 0;
            foreach (char c in arg)
            {
                if (c == '\\')
                {
                    ++backslashes;
                    continue;
                }
                if (c == '"')
                {
                    sb.Append('\\', backslashes * 2 + 1);
                    sb.Append('"');
                }
                else
                {
                    sb.Append('\\', backslashes);
                    sb.Append(c);
                }
                backslashes = 0;
            }
            sb.Append('\\', backslashes * 2);
            sb.Append('"');
            return sb.ToString();
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern uint GetShortPathNameW(string longPath, StringBuilder shortPath, uint bufferLength);

        /// <summary>
        /// The 8.3 short form of an existing path when the long one has
        /// characters outside printable ASCII and the short one does not; the
        /// path itself otherwise.  A defence for tools that turn their
        /// arguments into narrow strings somewhere on the way to the file
        /// system.  Never throws.
        /// </summary>
        public static string AsciiSafePath(string path)
        {
            if (string.IsNullOrEmpty(path) || IsPrintableAscii(path))
            {
                return path;
            }
            try
            {
                var sb = new StringBuilder(1024);
                uint n = GetShortPathNameW(path, sb, (uint)sb.Capacity);
                if (n > 0 && n < sb.Capacity)
                {
                    string shortPath = sb.ToString();
                    if (IsPrintableAscii(shortPath))
                    {
                        return shortPath;
                    }
                }
            }
            catch (Exception)
            {
                // No short names on this volume: keep the long path.
            }
            return path;
        }

        /// <summary>True when every character is printable 7-bit ASCII.</summary>
        public static bool IsPrintableAscii(string s)
        {
            if (s == null)
            {
                return true;
            }
            foreach (char c in s)
            {
                if (c < 0x20 || c > 0x7E)
                {
                    return false;
                }
            }
            return true;
        }
    }
}
