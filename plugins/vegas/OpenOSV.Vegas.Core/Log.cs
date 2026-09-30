// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Log.cs - the extension's rotating text log.

using System;
using System.Globalization;
using System.IO;
using System.Text;
using System.Threading;

namespace OpenOSV.Vegas.Core
{
    /// <summary>Severity of one log line.</summary>
    public enum LogLevel
    {
        /// <summary>Detail for a developer; written only when debug logging is on.</summary>
        Debug = 0,
        /// <summary>What happened.</summary>
        Info = 1,
        /// <summary>Something was skipped or fell back.</summary>
        Warn = 2,
        /// <summary>Something failed.</summary>
        Error = 3,
    }

    /// <summary>
    /// A small thread-safe, never-throwing file log with size rotation:
    /// <c>OpenOSVVegas.log</c>, then <c>.1</c> and <c>.2</c>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// A VEGAS extension that throws can take the user's whole session down,
    /// so the log is the one place every failure ends up, and logging itself
    /// must never fail loudly: every I/O error is swallowed.
    /// </para>
    /// <para>
    /// Each line is <c>2026-09-29 17:45:56.123 [INFO ] [t 1] message</c>.  The
    /// file is opened, appended and closed per line with
    /// <see cref="FileShare.ReadWrite"/>, so the user can open it in an editor
    /// while VEGAS runs; the extension logs a few lines per action, not per
    /// frame, so the open/close cost is irrelevant.
    /// </para>
    /// <para>
    /// Set the environment variable <c>OSV_VEGAS_LOG_LEVEL=debug</c> before
    /// starting VEGAS for debug lines.
    /// </para>
    /// </remarks>
    public static class Log
    {
        /// <summary>Size at which the file is rotated.</summary>
        public const long MaxBytes = 2L * 1024 * 1024;

        /// <summary>How many rotated files are kept beside the live one.</summary>
        public const int KeepFiles = 2;

        /// <summary>Serialises writes and rotation.</summary>
        private static readonly object Gate = new object();

        /// <summary>The lowest level written (read once from the environment).</summary>
        private static LogLevel _minLevel = ReadLevelFromEnvironment();

        /// <summary>The lowest level written.</summary>
        public static LogLevel MinLevel
        {
            get { return _minLevel; }
            set { _minLevel = value; }
        }

        /// <summary>The file being written (follows <see cref="AppPaths"/> overrides).</summary>
        public static string FilePath => AppPaths.LogFile;

        /// <summary>A debug line.</summary>
        public static void Debug(string message) => Write(LogLevel.Debug, message, null);

        /// <summary>An informational line.</summary>
        public static void Info(string message) => Write(LogLevel.Info, message, null);

        /// <summary>A warning line.</summary>
        public static void Warn(string message) => Write(LogLevel.Warn, message, null);

        /// <summary>A warning line with the exception that caused it.</summary>
        public static void Warn(string message, Exception ex) => Write(LogLevel.Warn, message, ex);

        /// <summary>An error line.</summary>
        public static void Error(string message) => Write(LogLevel.Error, message, null);

        /// <summary>An error line with the exception that caused it.</summary>
        public static void Error(string message, Exception ex) => Write(LogLevel.Error, message, ex);

        /// <summary>
        /// Write one line (and the exception's full text when given).  Never
        /// throws.
        /// </summary>
        /// <param name="level">Severity.</param>
        /// <param name="message">The line; null is written as "(null)".</param>
        /// <param name="ex">Optional exception.</param>
        public static void Write(LogLevel level, string message, Exception ex)
        {
            if (level < _minLevel)
            {
                return;
            }
            try
            {
                // ---- compose the line outside the lock ------------------------
                var sb = new StringBuilder(256);
                sb.Append(DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff", CultureInfo.InvariantCulture));
                sb.Append(" [").Append(LevelTag(level)).Append("] [t ");
                sb.Append(Thread.CurrentThread.ManagedThreadId.ToString(CultureInfo.InvariantCulture)).Append("] ");
                sb.Append(message ?? "(null)");
                if (ex != null)
                {
                    sb.Append(Environment.NewLine).Append("    ").Append(ex.ToString().Replace("\n", "\n    "));
                }
                sb.Append(Environment.NewLine);
                string line = sb.ToString();

                // ---- append (and rotate first when the file is full) ------------
                lock (Gate)
                {
                    string path = FilePath;
                    string dir = Path.GetDirectoryName(path);
                    if (!AppPaths.EnsureDir(dir))
                    {
                        return;
                    }
                    RotateIfNeeded(path);
                    using (var stream = new FileStream(path, FileMode.Append, FileAccess.Write, FileShare.ReadWrite))
                    using (var writer = new StreamWriter(stream, new UTF8Encoding(false)))
                    {
                        writer.Write(line);
                    }
                }
            }
            catch (Exception)
            {
                // Logging must never be the reason VEGAS goes down.
            }
        }

        /// <summary>
        /// Rename <c>log</c> to <c>log.1</c> (and <c>.1</c> to <c>.2</c>) when
        /// the live file has reached <see cref="MaxBytes"/>.  Caller holds the
        /// lock.
        /// </summary>
        /// <param name="path">The live log file.</param>
        internal static void RotateIfNeeded(string path)
        {
            try
            {
                var info = new FileInfo(path);
                if (!info.Exists || info.Length < MaxBytes)
                {
                    return;
                }
                // Oldest first: drop the last one, then shift every file up.
                string oldest = path + "." + KeepFiles.ToString(CultureInfo.InvariantCulture);
                if (File.Exists(oldest))
                {
                    File.Delete(oldest);
                }
                for (int i = KeepFiles - 1; i >= 1; --i)
                {
                    string from = path + "." + i.ToString(CultureInfo.InvariantCulture);
                    string to = path + "." + (i + 1).ToString(CultureInfo.InvariantCulture);
                    if (File.Exists(from))
                    {
                        File.Move(from, to);
                    }
                }
                File.Move(path, path + ".1");
            }
            catch (Exception)
            {
                // Another process holding the file open: keep appending; the
                // next line tries again.
            }
        }

        /// <summary>Five-character tag of a level, so the columns line up.</summary>
        private static string LevelTag(LogLevel level)
        {
            switch (level)
            {
                case LogLevel.Debug: return "DEBUG";
                case LogLevel.Info: return "INFO ";
                case LogLevel.Warn: return "WARN ";
                default: return "ERROR";
            }
        }

        /// <summary><c>OSV_VEGAS_LOG_LEVEL</c>: debug, info, warn or error (default info).</summary>
        private static LogLevel ReadLevelFromEnvironment()
        {
            try
            {
                string value = Environment.GetEnvironmentVariable("OSV_VEGAS_LOG_LEVEL");
                if (string.IsNullOrWhiteSpace(value))
                {
                    return LogLevel.Info;
                }
                switch (value.Trim().ToLowerInvariant())
                {
                    case "debug": return LogLevel.Debug;
                    case "warn":
                    case "warning": return LogLevel.Warn;
                    case "error": return LogLevel.Error;
                    default: return LogLevel.Info;
                }
            }
            catch (Exception)
            {
                return LogLevel.Info;
            }
        }
    }
}
