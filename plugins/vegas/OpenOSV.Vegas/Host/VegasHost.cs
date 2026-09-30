// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// VegasHost.cs - the one place the extension reaches into VEGAS for things
// that differ between VEGAS releases: plug-in lookup, time units, and the
// project's pixel format / HDR / 360 settings.

using System;
using System.Collections.Generic;
using System.Reflection;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Host
{
    /// <summary>
    /// The running VEGAS, as the extension sees it.  Set once from
    /// <see cref="OpenOsvModule.InitializeModule"/>.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Everything version-sensitive goes through reflection BY NAME here:
    /// <c>ProjectVideoProperties.PixelFormat</c>, <c>HDRMode</c> and
    /// <c>T360Output</c> and their enums.  A member that a newer VEGAS
    /// renumbers still reads right, and one an older VEGAS lacks reads as
    /// "unknown" instead of a MissingMethodException at JIT time - which
    /// in an extension is a crash report.
    /// </para>
    /// </remarks>
    internal static class VegasHost
    {
        /// <summary>The VEGAS instance handed to the module (null before initialisation).</summary>
        public static ScriptPortal.Vegas.Vegas App { get; set; }

        /// <summary>The active project, or null.</summary>
        public static Project Project
        {
            get
            {
                try
                {
                    return App?.Project;
                }
                catch (Exception ex)
                {
                    Log.Warn("vegas: reading the active project failed", ex);
                    return null;
                }
            }
        }

        /// <summary>VEGAS's main window, as a dialog owner (null when unknown).</summary>
        public static IWin32Window MainWindow
        {
            get
            {
                try
                {
                    return App?.MainWindow;
                }
                catch (Exception)
                {
                    return null;
                }
            }
        }

        // =====================================================================
        //  Plug-ins
        // =====================================================================

        /// <summary>
        /// OpenOSV Source in VEGAS's generator list, or null when the OpenFX
        /// bundle is not installed (or VEGAS has not scanned it since).
        /// </summary>
        public static PlugInNode FindSourceGenerator()
        {
            try
            {
                return FindPlugIn(App?.Generators, OfxIds.SourceUniqueId, OfxIds.SourcePluginId, OfxIds.SourceLabel);
            }
            catch (Exception ex)
            {
                Log.Warn("vegas: looking up OpenOSV Source failed", ex);
                return null;
            }
        }

        /// <summary>OpenOSV 360 Reframe in VEGAS's video FX list, or null.</summary>
        public static PlugInNode FindReframeFilter()
        {
            try
            {
                return FindPlugIn(App?.VideoFX, OfxIds.ReframeUniqueId, OfxIds.ReframePluginId, OfxIds.ReframeLabel);
            }
            catch (Exception ex)
            {
                Log.Warn("vegas: looking up OpenOSV 360 Reframe failed", ex);
                return null;
            }
        }

        /// <summary>
        /// A plug-in by VEGAS unique id ("{Svfx:org.openosv.OSVSource}"), then by
        /// any unique id containing the OpenFX identifier (VEGAS lists one entry
        /// per declared context), then by name - searched through every folder.
        /// </summary>
        internal static PlugInNode FindPlugIn(PlugInNode root, string uniqueId, string ofxId, string label)
        {
            if (root is null)
            {
                return null;
            }
            // ---- 1. the documented lookup --------------------------------------------
            try
            {
                PlugInNode direct = root.FindChildByUniqueID(uniqueId);
                if (direct is not null)
                {
                    return direct;
                }
            }
            catch (Exception)
            {
                // Some releases throw instead of returning null.
            }
            try
            {
                PlugInNode direct = root.GetChildByUniqueID(uniqueId);
                if (direct is not null)
                {
                    return direct;
                }
            }
            catch (Exception)
            {
                // Not there under that exact id.
            }

            // ---- 2. a walk: id containing the OpenFX identifier, then the label ------
            PlugInNode byName = null;
            var stack = new Stack<PlugInNode>();
            stack.Push(root);
            int visited = 0;
            while (stack.Count > 0 && visited < 20000)
            {
                PlugInNode node = stack.Pop();
                ++visited;
                IEnumerable<PlugInNode> children;
                try
                {
                    children = node;
                }
                catch (Exception)
                {
                    continue;
                }
                try
                {
                    foreach (PlugInNode child in children)
                    {
                        if (child is null)
                        {
                            continue;
                        }
                        string id = SafeString(() => child.UniqueID);
                        if (!string.IsNullOrEmpty(id) && id.IndexOf(ofxId, StringComparison.OrdinalIgnoreCase) >= 0 &&
                            !SafeBool(() => child.IsContainer))
                        {
                            return child;
                        }
                        if (byName is null && string.Equals(SafeString(() => child.Name), label, StringComparison.OrdinalIgnoreCase) &&
                            !SafeBool(() => child.IsContainer))
                        {
                            byName = child;
                        }
                        if (SafeBool(() => child.IsContainer))
                        {
                            stack.Push(child);
                        }
                    }
                }
                catch (Exception)
                {
                    // A folder that cannot be listed: skip it.
                }
            }
            return byName;
        }

        /// <summary>True when an effect is the given OpenOSV plug-in.</summary>
        public static bool IsPlugIn(Effect effect, string ofxId)
        {
            if (effect is null)
            {
                return false;
            }
            try
            {
                string id = effect.PlugIn?.UniqueID;
                return !string.IsNullOrEmpty(id) && id.IndexOf(ofxId, StringComparison.OrdinalIgnoreCase) >= 0;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // =====================================================================
        //  Time
        // =====================================================================

        private static long _unitsPerSecond;

        /// <summary>
        /// The scripting API's time units per second.  Its Timecode counts
        /// "nanos" of 100 ns (FromMilliseconds(1000).Nanos is 10 000 000); the
        /// value is measured once at run time, so an exact length stays exact
        /// even if a VEGAS release changes the unit.
        /// </summary>
        public static long UnitsPerSecond
        {
            get
            {
                if (_unitsPerSecond > 0)
                {
                    return _unitsPerSecond;
                }
                long measured = 0;
                try
                {
                    measured = Timecode.FromMilliseconds(1000.0).Nanos;
                }
                catch (Exception ex)
                {
                    Log.Warn("vegas: measuring the Timecode unit failed; assuming 100 ns", ex);
                }
                _unitsPerSecond = (measured >= 1000 && measured <= 1_000_000_000_000L) ? measured : 10_000_000L;
                return _unitsPerSecond;
            }
        }

        /// <summary>The exact length of <paramref name="frames"/> frames at <paramref name="fps"/>.</summary>
        public static Timecode LengthOf(long frames, Rational fps)
        {
            long units = TimeMath.FramesToUnits(frames, fps, UnitsPerSecond);
            if (units <= 0)
            {
                return null;
            }
            return Timecode.FromNanos(units);
        }

        /// <summary>A timecode from seconds (for positions that are not frame-exact).</summary>
        public static Timecode FromSeconds(double seconds)
        {
            if (double.IsNaN(seconds) || double.IsInfinity(seconds) || seconds < 0)
            {
                seconds = 0;
            }
            return Timecode.FromNanos((long)Math.Round(seconds * UnitsPerSecond));
        }

        /// <summary>Seconds of a timecode (0 for null).</summary>
        public static double Seconds(Timecode tc)
        {
            if (tc is null)
            {
                return 0.0;
            }
            try
            {
                return (double)tc.Nanos / UnitsPerSecond;
            }
            catch (Exception)
            {
                return 0.0;
            }
        }

        /// <summary>The shorter of two timecodes (null-safe).</summary>
        public static Timecode Min(Timecode a, Timecode b)
        {
            if (a is null) return b;
            if (b is null) return a;
            return a.Nanos <= b.Nanos ? a : b;
        }

        /// <summary>The playhead, or zero.</summary>
        public static Timecode Cursor
        {
            get
            {
                try
                {
                    return App?.Transport?.CursorPosition ?? App?.Cursor ?? Timecode.FromNanos(0);
                }
                catch (Exception)
                {
                    return Timecode.FromNanos(0);
                }
            }
        }

        // =====================================================================
        //  Project settings, by name
        // =====================================================================

        /// <summary>The project's pixel format enum NAME ("Int8Bit", "Float32BitFullRange"...), or null.</summary>
        public static string PixelFormatName(Project project) => EnumPropertyName(project?.Video, "PixelFormat");

        /// <summary>The project's HDR mode NAME ("HDR10", "HLG", "Off"), or null on a VEGAS without it.</summary>
        public static string HdrModeName(Project project) => EnumPropertyName(project?.Video, "HDRMode");

        /// <summary>The project's 360 output NAME ("On", "Off"), or null on a VEGAS without it.</summary>
        public static string T360OutputName(Project project) => EnumPropertyName(project?.Video, "T360Output");

        /// <summary>Turn the project's 360 output on or off.  False when this VEGAS has no such setting.</summary>
        public static bool SetT360Output(Project project, bool on) => SetEnumPropertyByName(project?.Video, "T360Output", on ? "On" : "Off");

        /// <summary>The levels the project works in.</summary>
        public static ProjectLevels Levels(Project project) => ProjectColour.LevelsFor(PixelFormatName(project));

        /// <summary>The project's frame size (0 x 0 when unknown).</summary>
        public static void FrameSize(Project project, out int width, out int height)
        {
            width = 0;
            height = 0;
            try
            {
                if (project?.Video is not null)
                {
                    width = project.Video.Width;
                    height = project.Video.Height;
                }
            }
            catch (Exception ex)
            {
                Log.Warn("vegas: reading the project frame size failed", ex);
            }
        }

        /// <summary>The name of an enum-typed property, read by reflection (null when absent).</summary>
        internal static string EnumPropertyName(object target, string property)
        {
            if (target is null)
            {
                return null;
            }
            try
            {
                PropertyInfo p = target.GetType().GetProperty(property, BindingFlags.Public | BindingFlags.Instance);
                object value = p?.GetValue(target, null);
                return value?.ToString();
            }
            catch (Exception ex)
            {
                Log.Debug("vegas: reading " + property + " failed: " + ex.Message);
                return null;
            }
        }

        /// <summary>Set an enum-typed property by member NAME (case-insensitive).  False when absent or refused.</summary>
        internal static bool SetEnumPropertyByName(object target, string property, string memberName)
        {
            if (target is null || string.IsNullOrEmpty(memberName))
            {
                return false;
            }
            try
            {
                PropertyInfo p = target.GetType().GetProperty(property, BindingFlags.Public | BindingFlags.Instance);
                if (p is null || !p.CanWrite || !p.PropertyType.IsEnum)
                {
                    return false;
                }
                foreach (string name in Enum.GetNames(p.PropertyType))
                {
                    if (string.Equals(name, memberName, StringComparison.OrdinalIgnoreCase))
                    {
                        p.SetValue(target, Enum.Parse(p.PropertyType, name), null);
                        return true;
                    }
                }
                return false;
            }
            catch (Exception ex)
            {
                Log.Warn("vegas: setting " + property + " = " + memberName + " failed", ex);
                return false;
            }
        }

        // =====================================================================
        //  Events, by name
        // =====================================================================

        /// <summary>
        /// Attach (or detach) a plain EventHandler to one of VEGAS's events by
        /// NAME.  Returns false, and logs once at debug level, when this VEGAS
        /// has no such event or its type is not EventHandler - an older release
        /// then simply sends fewer notifications.  Never throws.
        /// </summary>
        public static bool TryAttach(object source, string eventName, EventHandler handler, bool attach)
        {
            if (source is null || handler is null || string.IsNullOrEmpty(eventName))
            {
                return false;
            }
            try
            {
                EventInfo e = source.GetType().GetEvent(eventName, BindingFlags.Public | BindingFlags.Instance);
                if (e is null || e.EventHandlerType != typeof(EventHandler))
                {
                    Log.Debug("vegas: this VEGAS has no '" + eventName + "' event; skipped");
                    return false;
                }
                if (attach)
                {
                    e.AddEventHandler(source, handler);
                }
                else
                {
                    e.RemoveEventHandler(source, handler);
                }
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("vegas: " + (attach ? "attaching to" : "detaching from") + " '" + eventName + "' failed", ex);
                return false;
            }
        }

        // =====================================================================
        //  Skin colours
        // =====================================================================

        /// <summary>
        /// One of VEGAS's skin colours by <c>AppSkinColorID</c> member name
        /// ("WindowBackground", "WindowText", "Highlight"...), as a Win32
        /// COLORREF, or null.  Called through the scripting API's
        /// <c>IVegasCOM.GetAppSkinColor</c> by reflection so the enum is resolved
        /// by name in whatever VEGAS is running.
        /// </summary>
        public static int? SkinColor(string colorName)
        {
            try
            {
                object com = App is null ? null : (object)ScriptPortal.Vegas.Vegas.COM;
                if (com is null)
                {
                    return null;
                }
                MethodInfo m = typeof(IVegasCOM).GetMethod("GetAppSkinColor");
                if (m is null)
                {
                    return null;
                }
                ParameterInfo[] ps = m.GetParameters();
                if (ps.Length != 1 || !ps[0].ParameterType.IsEnum)
                {
                    return null;
                }
                object id = null;
                foreach (string name in Enum.GetNames(ps[0].ParameterType))
                {
                    if (string.Equals(name, colorName, StringComparison.OrdinalIgnoreCase))
                    {
                        id = Enum.Parse(ps[0].ParameterType, name);
                        break;
                    }
                }
                if (id is null)
                {
                    return null;
                }
                object result = m.Invoke(com, new[] { id });
                return result is int i ? i : (int?)null;
            }
            catch (Exception ex)
            {
                Log.Debug("vegas: skin colour " + colorName + " unavailable: " + ex.Message);
                return null;
            }
        }

        // ---- small guards ------------------------------------------------------------

        internal static string SafeString(Func<string> read)
        {
            try
            {
                return read();
            }
            catch (Exception)
            {
                return null;
            }
        }

        internal static bool SafeBool(Func<bool> read)
        {
            try
            {
                return read();
            }
            catch (Exception)
            {
                return false;
            }
        }
    }
}
