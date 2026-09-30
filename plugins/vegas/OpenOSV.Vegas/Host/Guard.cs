// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Guard.cs - nothing the extension does may take VEGAS down with it.

using System;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.UI;

namespace OpenOSV.Vegas.Host
{
    /// <summary>
    /// Wrappers every command, button and VEGAS event handler runs inside.
    /// </summary>
    /// <remarks>
    /// <para>
    /// An exception that escapes an extension's handler lands in VEGAS's own
    /// message loop, and VEGAS's answer to that is a crash report and a lost
    /// session.  So every entry point from VEGAS into this assembly goes
    /// through <see cref="Run"/> (user actions: log, then tell the user in one
    /// friendly dialog) or <see cref="Quietly"/> (VEGAS events: log only - a
    /// dialog popping up from a media-pool event would be worse than the bug).
    /// </para>
    /// <para>
    /// The body is invoked through a delegate on purpose: a VEGAS release that
    /// lacks a member the body uses fails when the BODY is compiled, which
    /// happens inside the try below, not in the caller.
    /// </para>
    /// </remarks>
    internal static class Guard
    {
        /// <summary>Run a user action; any exception is logged and shown once.</summary>
        /// <param name="action">What the user asked for ("Import OSV").</param>
        /// <param name="body">The work.</param>
        public static void Run(string action, Action body)
        {
            if (body is null)
            {
                return;
            }
            try
            {
                body();
            }
            catch (Exception ex)
            {
                Log.Error(action + " failed", ex);
                Notifier.Status(action + " failed: " + ex.Message, StatusKind.Error);
                try
                {
                    MessageDialog.Show(VegasHost.MainWindow, Copy.ProductName, Copy.Unexpected(action, ex), MessageKind.Error);
                }
                catch (Exception inner)
                {
                    Log.Error("showing the error dialog failed too", inner);
                }
            }
        }

        /// <summary>Run an event handler; any exception is only logged.</summary>
        public static void Quietly(string what, Action body)
        {
            if (body is null)
            {
                return;
            }
            try
            {
                body();
            }
            catch (Exception ex)
            {
                Log.Warn(what + " failed", ex);
            }
        }

        /// <summary>Run a function; any exception is logged and <paramref name="fallback"/> returned.</summary>
        public static T Try<T>(string what, Func<T> body, T fallback)
        {
            if (body is null)
            {
                return fallback;
            }
            try
            {
                return body();
            }
            catch (Exception ex)
            {
                Log.Warn(what + " failed", ex);
                return fallback;
            }
        }

        /// <summary>Wrap an event handler so it can never throw into VEGAS.</summary>
        public static EventHandler Handler(string what, Action body) => (sender, args) => Quietly(what, body);

        /// <summary>Wrap a click handler so it can never throw into WinForms.</summary>
        public static EventHandler Click(string action, Action body) => (sender, args) => Run(action, body);

        /// <summary>
        /// Show the result of an action: the status line always; a dialog when
        /// something needs reading (errors, or details the line cannot hold).
        /// </summary>
        public static void Report(ActionResult result)
        {
            if (result is null)
            {
                return;
            }
            Notifier.Status(result.Summary, result.Kind);
            // Whatever the action did - or declined to do - the panel re-reads
            // the project, so no control keeps showing a choice that did not
            // take (a proxy switch with no .LRF beside the clips, say).
            Notifier.RaiseClipsChanged();
            if (result.ShowDialog)
            {
                try
                {
                    MessageDialog.Show(VegasHost.MainWindow, result.Title, result.DialogText, result.Kind == StatusKind.Error ? MessageKind.Error : MessageKind.Warning);
                }
                catch (Exception ex)
                {
                    Log.Error("showing a result dialog failed", ex);
                }
            }
        }

        /// <summary>A modal owner that is always valid for WinForms (VEGAS's main window, else null).</summary>
        public static IWin32Window Owner => VegasHost.MainWindow;
    }
}
