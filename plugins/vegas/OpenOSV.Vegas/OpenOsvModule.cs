// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OpenOsvModule.cs - the VEGAS Pro Application Extension's entry point.
//
// ===========================================================================
//  What VEGAS does with this class
// ===========================================================================
// At start-up VEGAS scans %ProgramData%\VEGAS Pro\Application Extensions\
// (and its per-version sub-folders) for assemblies with a public type that
// implements the scripting API's ICustomCommandModule, creates one, calls
// InitializeModule(vegas) and then GetCustomCommands() for the menu
// entries.  Commands in CommandCategory.Tools appear under Tools >
// Extensions, those in CommandCategory.View under View > Extensions.
//
// Every handler below runs inside Guard: an exception escaping into VEGAS's
// message loop would end the user's session.

using System;
using System.Collections;
using System.Collections.Generic;
using System.IO;
using System.Reflection;
using OpenOSV.Vegas.Actions;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using OpenOSV.Vegas.UI;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas
{
    /// <summary>The extension module VEGAS loads.</summary>
    public sealed class OpenOsvModule : ICustomCommandModule
    {
        /// <summary>The dock panel's instance name (VEGAS persists its layout by it).</summary>
        internal const string DockName = "OpenOSV";

        private readonly List<CustomCommand> _commands = new List<CustomCommand>();
        private CustomCommand _panelCommand;

        /// <summary>
        /// Runs before anything touches OpenOSV.Vegas.Core: installs the fallback
        /// that finds Core beside this assembly (see <see cref="CoreResolver"/>).
        /// </summary>
        static OpenOsvModule()
        {
            CoreResolver.Install();
        }

        /// <summary>Called by VEGAS once, at start-up.</summary>
        public void InitializeModule(ScriptPortal.Vegas.Vegas vegas)
        {
            Guard.Quietly("initialising the OpenOSV extension", () =>
            {
                VegasHost.App = vegas;
                Theme.Refresh();
                Log.Info("OpenOSV for VEGAS " + typeof(OpenOsvModule).Assembly.GetName().Version + " loaded into " +
                         VegasHost.SafeString(() => vegas.Version) + " (" + VegasHost.SafeString(() => vegas.LongAppName) + ")");
            });
        }

        /// <summary>Called by VEGAS for the menu entries.</summary>
        public ICollection GetCustomCommands()
        {
            _commands.Clear();
            Guard.Quietly("building the OpenOSV menus", () =>
            {
                // ---- View > Extensions > OpenOSV: the dock panel ------------------------------
                _panelCommand = new CustomCommand(CommandCategory.View, "OpenOSV.Panel") { DisplayName = "OpenOSV" };
                _panelCommand.Invoked += Guard.Handler("opening the OpenOSV panel", () => OpenPanel());
                _panelCommand.MenuPopup += Guard.Handler("the OpenOSV panel menu", () =>
                {
                    _panelCommand.Checked = VegasHost.App is not null && VegasHost.App.FindDockView(DockName);
                });
                _commands.Add(_panelCommand);

                // ---- Tools > Extensions > OpenOSV > ... ---------------------------------------------
                var root = new CustomCommand(CommandCategory.Tools, "OpenOSV.Tools") { DisplayName = "OpenOSV" };
                root.AddChild(Command("OpenOSV.Import", "Import OSV...", () => ImportAction.RunWithDialog()));
                root.AddChild(Command("OpenOSV.Proxies", "Edit with LRF proxies", () => Guard.Report(ProxyAction.Run(true))));
                root.AddChild(Command("OpenOSV.FullQuality", "Full quality", () => Guard.Report(ProxyAction.Run(false))));
                root.AddChild(Command("OpenOSV.Setup360", "360 project setup", () => Guard.Report(ProjectActions.Setup360())));
                root.AddChild(Command("OpenOSV.Reframe", "Apply 360 Reframe to selected events", () => Guard.Report(ProjectActions.ApplyReframeFilter())));

                var looks = new CustomCommand(CommandCategory.Tools, "OpenOSV.Looks") { DisplayName = "Framing look" };
                foreach (FramingLook look in DjiCamera.Looks)
                {
                    string id = look.Id;
                    looks.AddChild(Command("OpenOSV.Look." + id, look.Label, () => Guard.Report(CameraActions.ApplyLook(id, null))));
                }
                root.AddChild(looks);

                var easing = new CustomCommand(CommandCategory.Tools, "OpenOSV.Easing") { DisplayName = "Keyframe easing" };
                foreach (EasingPreset preset in DjiCamera.Easings)
                {
                    string id = preset.Id;
                    easing.AddChild(Command("OpenOSV.Easing." + id, preset.Label, () => Guard.Report(CameraActions.ApplyEasing(id, null))));
                }
                root.AddChild(easing);

                var stab = new CustomCommand(CommandCategory.Tools, "OpenOSV.Stabilisation") { DisplayName = "Stabilisation" };
                stab.AddChild(Command("OpenOSV.Stab.Both", "RockSteady + Horizon Leveling", () => Guard.Report(CameraActions.ApplyStabilisation(true, true, null))));
                stab.AddChild(Command("OpenOSV.Stab.RockSteady", "RockSteady only", () => Guard.Report(CameraActions.ApplyStabilisation(true, false, null))));
                stab.AddChild(Command("OpenOSV.Stab.Horizon", "Horizon Leveling only", () => Guard.Report(CameraActions.ApplyStabilisation(false, true, null))));
                stab.AddChild(Command("OpenOSV.Stab.Off", "Off", () => Guard.Report(CameraActions.ApplyStabilisation(false, false, null))));
                root.AddChild(stab);

                root.AddChild(Command("OpenOSV.Unique", "Make framing unique", () => Guard.Report(MediaActions.MakeFramingUnique())));
                root.AddChild(Command("OpenOSV.Relink", "Relink moved OSVs...", () => Guard.Report(MediaActions.Relink())));
                root.AddChild(Command("OpenOSV.Levels", "Match levels to project", () => Guard.Report(ClipEdit.MatchLevels())));
                root.AddChild(Command("OpenOSV.OpenPanel", "Show the OpenOSV panel", () => OpenPanel()));
                _commands.Add(root);
            });
            return _commands;
        }

        /// <summary>A Tools command whose handler runs inside Guard.</summary>
        private static CustomCommand Command(string name, string display, Action body)
        {
            var cmd = new CustomCommand(CommandCategory.Tools, name) { DisplayName = display };
            cmd.Invoked += Guard.Click(display.TrimEnd('.'), body);
            try
            {
                cmd.CanAddToKeybindings = true;
                cmd.CanAddToToolbar = true;
            }
            catch (Exception)
            {
                // Optional on older VEGAS releases.
            }
            return cmd;
        }

        /// <summary>Show the dock panel, creating it the first time.</summary>
        private void OpenPanel()
        {
            ScriptPortal.Vegas.Vegas app = VegasHost.App;
            if (app is null)
            {
                return;
            }
            if (app.ActivateDockView(DockName))
            {
                return;
            }
            var dock = new OpenOsvDock
            {
                AutoLoadCommand = _panelCommand,
                PersistDockWindowState = true,
            };
            app.LoadDockView(dock);
        }
    }

    /// <summary>
    /// A fallback that finds OpenOSV.Vegas.Core.dll beside this assembly.
    /// </summary>
    /// <remarks>
    /// <para>
    /// When VEGAS loads an extension from a path, .NET probes that path's
    /// folder for its dependencies and this handler is never asked.  It only
    /// matters for a host that loads the extension some other way (from
    /// bytes, or from another context); then .NET raises AssemblyResolve for
    /// Core and this answers with the file beside the extension.
    /// </para>
    /// <para>
    /// It answers for exactly one name, never recurses (a thread-static
    /// guard), and uses nothing from Core itself - it has to work before
    /// Core is loaded.
    /// </para>
    /// </remarks>
    internal static class CoreResolver
    {
        private const string CoreName = "OpenOSV.Vegas.Core";

        [ThreadStatic]
        private static bool _resolving;

        private static bool _installed;

        public static void Install()
        {
            try
            {
                if (_installed)
                {
                    return;
                }
                _installed = true;
                AppDomain.CurrentDomain.AssemblyResolve += Resolve;
            }
            catch (Exception)
            {
                // Without the fallback the normal probing still applies.
            }
        }

        private static Assembly Resolve(object sender, ResolveEventArgs args)
        {
            if (_resolving || args?.Name is null)
            {
                return null;
            }
            string name = args.Name;
            int comma = name.IndexOf(',');
            string simple = comma >= 0 ? name.Substring(0, comma).Trim() : name.Trim();
            if (!string.Equals(simple, CoreName, StringComparison.OrdinalIgnoreCase))
            {
                return null;
            }
            try
            {
                _resolving = true;
                foreach (Assembly loaded in AppDomain.CurrentDomain.GetAssemblies())
                {
                    if (string.Equals(loaded.GetName().Name, CoreName, StringComparison.OrdinalIgnoreCase))
                    {
                        return loaded;
                    }
                }
                string here = typeof(CoreResolver).Assembly.Location;
                if (string.IsNullOrEmpty(here))
                {
                    return null;
                }
                string candidate = Path.Combine(Path.GetDirectoryName(here) ?? string.Empty, CoreName + ".dll");
                return File.Exists(candidate) ? Assembly.LoadFrom(candidate) : null;
            }
            catch (Exception)
            {
                return null;
            }
            finally
            {
                _resolving = false;
            }
        }
    }
}
