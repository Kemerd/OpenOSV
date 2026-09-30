// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ActionResult.cs - what an action tells the user, and the channel it goes
// through.

using System;
using System.Collections.Generic;
using System.Text;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas
{
    /// <summary>How a status line is coloured.</summary>
    public enum StatusKind
    {
        /// <summary>Neutral information.</summary>
        Info,
        /// <summary>It worked.</summary>
        Success,
        /// <summary>It worked, with something to know.</summary>
        Warning,
        /// <summary>It failed.</summary>
        Error,
    }

    /// <summary>The outcome of one command: a line for the status bar, details for a dialog.</summary>
    internal sealed class ActionResult
    {
        private readonly List<string> _details = new List<string>();

        /// <summary>Create a result.</summary>
        public ActionResult(string title, string summary, StatusKind kind)
        {
            Title = title ?? Copy.ProductName;
            Summary = summary ?? string.Empty;
            Kind = kind;
        }

        /// <summary>Dialog title (the action's name).</summary>
        public string Title { get; }

        /// <summary>The one-line summary.</summary>
        public string Summary { get; set; }

        /// <summary>Its colour.</summary>
        public StatusKind Kind { get; set; }

        /// <summary>Per-item details (one line each: "CAM_0001.OSV: why").</summary>
        public IReadOnlyList<string> Details => _details;

        /// <summary>Force a dialog even without errors (for a message that must be read).</summary>
        public bool ForceDialog { get; set; }

        /// <summary>True when the result deserves a dialog: errors, warnings with details, or forced.</summary>
        public bool ShowDialog => ForceDialog || (_details.Count > 0 && Kind != StatusKind.Success && Kind != StatusKind.Info);

        /// <summary>Add a detail line.</summary>
        public void Add(string detail)
        {
            if (!string.IsNullOrWhiteSpace(detail))
            {
                _details.Add(detail.Trim());
            }
        }

        /// <summary>The dialog text: the summary, then the details (at most 30 lines).</summary>
        public string DialogText
        {
            get
            {
                var sb = new StringBuilder(Summary);
                if (_details.Count > 0)
                {
                    sb.Append("\n");
                    int shown = 0;
                    foreach (string d in _details)
                    {
                        if (shown++ >= 30)
                        {
                            sb.Append("\n... and ").Append(_details.Count - 30).Append(" more (see the log).");
                            break;
                        }
                        sb.Append("\n- ").Append(d);
                    }
                }
                return sb.ToString();
            }
        }

        /// <summary>A plain one-line result.</summary>
        public static ActionResult Line(string title, string text, StatusKind kind) => new ActionResult(title, text, kind);

        /// <summary>A result that must be read in a dialog.</summary>
        public static ActionResult Blocking(string title, string text, StatusKind kind = StatusKind.Warning) =>
            new ActionResult(title, text, kind) { ForceDialog = true };
    }

    /// <summary>
    /// Where status lines go: the dock panel when it is open (it shows the
    /// last one with a timestamp), and always the log.
    /// </summary>
    internal static class Notifier
    {
        /// <summary>Raised on the UI thread for every status line.</summary>
        public static event Action<string, StatusKind> StatusChanged;

        /// <summary>Raised when something changed the project's OpenOSV clips (refresh the panel).</summary>
        public static event Action ClipsChanged;

        /// <summary>Post a status line.  Never throws.</summary>
        public static void Status(string text, StatusKind kind)
        {
            try
            {
                Log.Info("status: " + text);
                StatusChanged?.Invoke(text, kind);
            }
            catch (Exception ex)
            {
                Log.Warn("a status listener threw", ex);
            }
        }

        /// <summary>Tell listeners the clips changed.  Never throws.</summary>
        public static void RaiseClipsChanged()
        {
            try
            {
                ClipsChanged?.Invoke();
            }
            catch (Exception ex)
            {
                Log.Warn("a clips-changed listener threw", ex);
            }
        }
    }
}
