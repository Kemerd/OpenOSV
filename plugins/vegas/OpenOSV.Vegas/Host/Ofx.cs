// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Ofx.cs - reading and writing OpenFX parameters through the VEGAS scripting
// API, defensively.

using System;
using System.Collections.Generic;
using System.Linq;
using OpenOSV.Vegas.Core;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Host
{
    /// <summary>
    /// Typed, never-throwing access to an effect's OpenFX parameters.
    /// </summary>
    /// <remarks>
    /// <para>
    /// Every write ends with <c>OFXParameter.ParameterChanged()</c>.  Nothing
    /// documents whether setting <c>.Value</c> alone makes VEGAS send the
    /// plug-in its instance-changed action - and OpenOSV Source re-opens its
    /// clip and refreshes its Clip read-out from exactly that action - so the
    /// extension always says so explicitly.
    /// </para>
    /// <para>
    /// A write to an ANIMATED parameter with a key time sets a keyframe there
    /// (added, or updated if one exists), the Premiere panel's rule: a user
    /// who keyframed Tilt expects a look to key the playhead, not to flatten
    /// the animation.  Without a key time, or on a static parameter, the
    /// value itself is set.
    /// </para>
    /// <para>
    /// A parameter the plug-in does not define (outputLevels on a bundle
    /// older than WP-V-OFX) is simply skipped: the write returns false and the
    /// caller decides whether that matters.
    /// </para>
    /// </remarks>
    internal static class Ofx
    {
        /// <summary>The OpenFX side of an effect, or null.</summary>
        public static OFXEffect Of(Effect effect)
        {
            try
            {
                return (effect is not null && effect.IsOFX) ? effect.OFXEffect : null;
            }
            catch (Exception ex)
            {
                Log.Warn("ofx: reading an effect's OpenFX side failed", ex);
                return null;
            }
        }

        /// <summary>A parameter by name, or null.</summary>
        public static OFXParameter Find(OFXEffect fx, string name)
        {
            if (fx is null || string.IsNullOrEmpty(name))
            {
                return null;
            }
            try
            {
                return fx.FindParameterByName(name);
            }
            catch (Exception)
            {
                // Some releases throw for an unknown name.
                return null;
            }
        }

        /// <summary>True when the effect defines the parameter.</summary>
        public static bool Has(OFXEffect fx, string name) => Find(fx, name) is not null;

        // =====================================================================
        //  Reads
        // =====================================================================

        public static string GetString(OFXEffect fx, string name, string fallback = "")
        {
            try
            {
                return Find(fx, name) is OFXStringParameter p ? (p.Value ?? fallback) : fallback;
            }
            catch (Exception)
            {
                return fallback;
            }
        }

        /// <summary>A choice's 0-based index, or <paramref name="fallback"/>.</summary>
        public static int GetChoice(OFXEffect fx, string name, int fallback)
        {
            try
            {
                if (Find(fx, name) is OFXChoiceParameter p && p.Value is not null)
                {
                    int index = p.Value.Index;
                    return index >= 0 ? index : fallback;
                }
            }
            catch (Exception)
            {
                // Fall through.
            }
            return fallback;
        }

        public static int GetInt(OFXEffect fx, string name, int fallback)
        {
            try
            {
                return Find(fx, name) is OFXIntegerParameter p ? p.Value : fallback;
            }
            catch (Exception)
            {
                return fallback;
            }
        }

        public static double GetDouble(OFXEffect fx, string name, double fallback)
        {
            try
            {
                return Find(fx, name) is OFXDoubleParameter p ? p.Value : fallback;
            }
            catch (Exception)
            {
                return fallback;
            }
        }

        public static bool GetBool(OFXEffect fx, string name, bool fallback)
        {
            try
            {
                return Find(fx, name) is OFXBooleanParameter p ? p.Value : fallback;
            }
            catch (Exception)
            {
                return fallback;
            }
        }

        /// <summary>True when the parameter exists and is keyframed.</summary>
        public static bool IsAnimated(OFXEffect fx, string name)
        {
            try
            {
                OFXParameter p = Find(fx, name);
                return p is not null && p.IsAnimated;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // =====================================================================
        //  Writes
        // =====================================================================

        public static bool SetString(OFXEffect fx, string name, string value)
        {
            if (!(Find(fx, name) is OFXStringParameter p))
            {
                return Missing(name);
            }
            return Write(name, p, () => p.Value = value ?? string.Empty);
        }

        /// <summary>
        /// Set a choice by its 0-based index.  The host's own item list is
        /// searched for the expected label first, so a plug-in that ever
        /// reorders a popup is caught by label rather than written blind.
        /// </summary>
        public static bool SetChoice(OFXEffect fx, string name, int index0, ChoiceList expected, Timecode keyTime = null)
        {
            if (!(Find(fx, name) is OFXChoiceParameter p))
            {
                return Missing(name);
            }
            OFXChoice choice = ChoiceFor(p, index0, expected);
            if (choice is null)
            {
                Log.Warn("ofx: '" + name + "' has no entry " + index0 + (expected is not null ? " ('" + expected[index0] + "')" : string.Empty));
                return false;
            }
            return Write(name, p, () =>
            {
                if (keyTime is not null && p.IsAnimated)
                {
                    p.SetValueAtTime(keyTime, choice);
                }
                else
                {
                    p.Value = choice;
                }
            });
        }

        public static bool SetInt(OFXEffect fx, string name, int value, Timecode keyTime = null)
        {
            if (!(Find(fx, name) is OFXIntegerParameter p))
            {
                return Missing(name);
            }
            return Write(name, p, () =>
            {
                if (keyTime is not null && p.IsAnimated)
                {
                    p.SetValueAtTime(keyTime, value);
                }
                else
                {
                    p.Value = value;
                }
            });
        }

        public static bool SetDouble(OFXEffect fx, string name, double value, Timecode keyTime = null)
        {
            if (double.IsNaN(value) || double.IsInfinity(value))
            {
                Log.Warn("ofx: refusing to write " + value + " into '" + name + "'");
                return false;
            }
            if (!(Find(fx, name) is OFXDoubleParameter p))
            {
                return Missing(name);
            }
            return Write(name, p, () =>
            {
                if (keyTime is not null && p.IsAnimated)
                {
                    p.SetValueAtTime(keyTime, value);
                }
                else
                {
                    p.Value = value;
                }
            });
        }

        public static bool SetBool(OFXEffect fx, string name, bool value, Timecode keyTime = null)
        {
            if (!(Find(fx, name) is OFXBooleanParameter p))
            {
                return Missing(name);
            }
            return Write(name, p, () =>
            {
                if (keyTime is not null && p.IsAnimated)
                {
                    p.SetValueAtTime(keyTime, value);
                }
                else
                {
                    p.Value = value;
                }
            });
        }

        /// <summary>The host's choice for an index (by expected label first).</summary>
        private static OFXChoice ChoiceFor(OFXChoiceParameter p, int index0, ChoiceList expected)
        {
            OFXChoice[] choices;
            try
            {
                choices = p.Choices;
            }
            catch (Exception)
            {
                return null;
            }
            if (choices is null || choices.Length == 0)
            {
                return null;
            }
            string label = expected?[index0];
            if (label is not null)
            {
                foreach (OFXChoice c in choices)
                {
                    if (c is not null && string.Equals((c.Name ?? string.Empty).Trim(), label, StringComparison.OrdinalIgnoreCase))
                    {
                        return c;
                    }
                }
            }
            foreach (OFXChoice c in choices)
            {
                if (c is not null && c.Index == index0)
                {
                    return c;
                }
            }
            return (index0 >= 0 && index0 < choices.Length) ? choices[index0] : null;
        }

        /// <summary>Run one write and tell the plug-in about it.</summary>
        private static bool Write(string name, OFXParameter p, Action set)
        {
            try
            {
                set();
            }
            catch (Exception ex)
            {
                Log.Warn("ofx: writing '" + name + "' failed", ex);
                return false;
            }
            try
            {
                p.ParameterChanged();
            }
            catch (Exception ex)
            {
                // The value is in; only the notification failed.
                Log.Warn("ofx: ParameterChanged for '" + name + "' failed", ex);
            }
            return true;
        }

        private static bool Missing(string name)
        {
            Log.Debug("ofx: no parameter '" + name + "' on this effect; skipped");
            return false;
        }

        // =====================================================================
        //  Copy every parameter, keyframes included (Make framing unique)
        // =====================================================================

        /// <summary>
        /// Copy every value parameter of <paramref name="from"/> into
        /// <paramref name="to"/> (same plug-in), keyframes and their
        /// interpolation included.  Groups, pages and buttons carry nothing.
        /// Returns how many parameters were copied.
        /// </summary>
        public static int CopyAll(OFXEffect from, OFXEffect to)
        {
            if (from is null || to is null)
            {
                return 0;
            }
            int copied = 0;
            List<OFXParameter> source;
            try
            {
                source = from.Parameters.ToList();
            }
            catch (Exception ex)
            {
                Log.Warn("ofx: listing parameters to copy failed", ex);
                return 0;
            }
            foreach (OFXParameter s in source)
            {
                string name = VegasHost.SafeString(() => s?.Name);
                if (string.IsNullOrEmpty(name))
                {
                    continue;
                }
                OFXParameter d = Find(to, name);
                if (d is null)
                {
                    continue;
                }
                try
                {
                    bool ok = false;
                    if (s is OFXDoubleParameter sd && d is OFXDoubleParameter dd)
                    {
                        ok = CopyTyped<OFXDoubleParameter, OFXDoubleKeyframe, double>(
                            sd, dd, k => k.Value, (p, t, v) => p.SetValueAtTime(t, v), p => p.Value, (p, v) => p.Value = v);
                    }
                    else if (s is OFXIntegerParameter si && d is OFXIntegerParameter di)
                    {
                        ok = CopyTyped<OFXIntegerParameter, OFXIntegerKeyframe, int>(
                            si, di, k => k.Value, (p, t, v) => p.SetValueAtTime(t, v), p => p.Value, (p, v) => p.Value = v);
                    }
                    else if (s is OFXBooleanParameter sb && d is OFXBooleanParameter db)
                    {
                        ok = CopyTyped<OFXBooleanParameter, OFXBooleanKeyframe, bool>(
                            sb, db, k => k.Value, (p, t, v) => p.SetValueAtTime(t, v), p => p.Value, (p, v) => p.Value = v);
                    }
                    else if (s is OFXStringParameter ss && d is OFXStringParameter ds)
                    {
                        ok = CopyTyped<OFXStringParameter, OFXStringKeyframe, string>(
                            ss, ds, k => k.Value, (p, t, v) => p.SetValueAtTime(t, v), p => p.Value, (p, v) => p.Value = v);
                    }
                    else if (s is OFXChoiceParameter sc && d is OFXChoiceParameter dc)
                    {
                        // A choice value belongs to its own parameter: map it by index.
                        Func<OFXChoice, OFXChoice> map = c => c is null ? null : ChoiceFor(dc, c.Index, null);
                        ok = CopyTyped<OFXChoiceParameter, OFXChoiceKeyframe, OFXChoice>(
                            sc, dc, k => map(k.Value), (p, t, v) => { if (v is not null) p.SetValueAtTime(t, v); },
                            p => map(p.Value), (p, v) => { if (v is not null) p.Value = v; });
                    }
                    if (ok)
                    {
                        ++copied;
                        try
                        {
                            d.ParameterChanged();
                        }
                        catch (Exception)
                        {
                            // The value is in.
                        }
                    }
                }
                catch (Exception ex)
                {
                    Log.Warn("ofx: copying '" + name + "' failed", ex);
                }
            }
            return copied;
        }

        /// <summary>
        /// Copy one typed parameter: its keyframes (time, value, interpolation)
        /// when it is animated, else its value.
        /// </summary>
        private static bool CopyTyped<TParam, TKey, TValue>(TParam s, TParam d, Func<TKey, TValue> keyValue,
                                                             Action<TParam, Timecode, TValue> setAt,
                                                             Func<TParam, TValue> getValue, Action<TParam, TValue> setValue)
            where TParam : OFXParameter
            where TKey : OFXKeyframe
        {
            List<TKey> keys = KeysOf<TKey>(s);
            if (!s.IsAnimated || keys.Count == 0)
            {
                setValue(d, getValue(s));
                return true;
            }
            // ---- animated: one keyframe per source keyframe --------------------------
            d.IsAnimated = true;
            var times = new HashSet<long>();
            foreach (TKey k in keys)
            {
                setAt(d, k.Time, keyValue(k));
                times.Add(k.Time.Nanos);
            }
            // ---- drop keys the destination had that the source does not ------------------
            // (turning animation on may itself create one at the current time)
            foreach (TKey extra in KeysOf<TKey>(d).Where(k => !times.Contains(k.Time.Nanos)).ToList())
            {
                RemoveKey(d, extra);
            }
            // ---- interpolation, key by key ---------------------------------------------------
            List<TKey> dst = KeysOf<TKey>(d);
            foreach (TKey sk in keys)
            {
                TKey dk = dst.FirstOrDefault(k => k.Time.Nanos == sk.Time.Nanos);
                if (dk is not null)
                {
                    try
                    {
                        dk.Interpolation = sk.Interpolation;
                    }
                    catch (Exception)
                    {
                        // A host that fixes interpolation per parameter type.
                    }
                }
            }
            return true;
        }

        /// <summary>A parameter's keyframes as a list (empty when it has none or cannot say).</summary>
        private static List<TKey> KeysOf<TKey>(OFXParameter p) where TKey : OFXKeyframe
        {
            var list = new List<TKey>();
            try
            {
                object keyframes = p.GetType().GetProperty("Keyframes")?.GetValue(p, null);
                if (keyframes is System.Collections.IEnumerable items)
                {
                    foreach (object o in items)
                    {
                        if (o is TKey k)
                        {
                            list.Add(k);
                        }
                    }
                }
            }
            catch (Exception)
            {
                // No keyframes to report.
            }
            return list;
        }

        private static void RemoveKey(OFXParameter p, OFXKeyframe key)
        {
            try
            {
                object keyframes = p.GetType().GetProperty("Keyframes")?.GetValue(p, null);
                if (keyframes is System.Collections.IList list)
                {
                    list.Remove(key);
                }
            }
            catch (Exception ex)
            {
                Log.Debug("ofx: removing a stray keyframe failed: " + ex.Message);
            }
        }
    }
}
