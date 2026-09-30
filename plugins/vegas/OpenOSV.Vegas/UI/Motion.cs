// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Motion.cs - critically damped springs, one timer that drives them, and
// Windows' "Animation effects" setting.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// A critically damped spring: the fastest motion to a target that never
    /// overshoots - a switch knob that lands, a selection that glides, text
    /// that settles.
    /// </summary>
    /// <remarks>
    /// Integrated with the exact solution of x'' = -w^2 x - 2 w x' (w = 2 pi /
    /// response), not with Euler steps, so a dropped frame (VEGAS busy
    /// rendering a preview) cannot make it jump or blow up: any dt lands on the
    /// curve the spring would have followed anyway.
    /// </remarks>
    internal sealed class Spring
    {
        /// <summary>Create a spring resting at <paramref name="value"/>.</summary>
        /// <param name="value">Start and target.</param>
        /// <param name="response">Seconds to settle, roughly (0.32 controls, 0.45 text).</param>
        public Spring(double value, double response = 0.32)
        {
            Value = value;
            Target = value;
            Response = response > 0.02 ? response : 0.02;
        }

        /// <summary>The current value.</summary>
        public double Value { get; private set; }

        /// <summary>The current velocity (units per second).</summary>
        public double Velocity { get; private set; }

        /// <summary>Where it is going.</summary>
        public double Target { get; set; }

        /// <summary>Settling time scale, in seconds.</summary>
        public double Response { get; set; }

        /// <summary>True once it sits on its target.</summary>
        public bool AtRest => Math.Abs(Value - Target) < 1e-3 && Math.Abs(Velocity) < 1e-2;

        /// <summary>Advance by <paramref name="dt"/> seconds.</summary>
        public void Step(double dt)
        {
            if (dt <= 0 || double.IsNaN(dt))
            {
                return;
            }
            double w = 2.0 * Math.PI / Response;
            double x0 = Value - Target;
            double v0 = Velocity;
            double e = Math.Exp(-w * dt);
            double b = v0 + w * x0;
            double x = (x0 + b * dt) * e;
            double v = (v0 - w * b * dt) * e;
            Value = Target + x;
            Velocity = v;
            if (AtRest || double.IsNaN(Value) || double.IsInfinity(Value))
            {
                Snap(Target);
            }
        }

        /// <summary>Jump to a value and stop there.</summary>
        public void Snap(double value)
        {
            Value = value;
            Target = value;
            Velocity = 0.0;
        }
    }

    /// <summary>
    /// The one UI-thread timer every spring runs on.  It runs only while
    /// something moves, so an idle panel costs VEGAS nothing.
    /// </summary>
    internal static class Animator
    {
        private sealed class Entry
        {
            public Control Owner;
            public Spring Spring;
            public Action OnStep;
        }

        private static readonly List<Entry> Active = new List<Entry>();
        private static readonly Stopwatch Clock = Stopwatch.StartNew();
        private static Timer _timer;
        private static double _last;

        /// <summary>
        /// Move <paramref name="spring"/> to <paramref name="target"/>, repainting
        /// <paramref name="owner"/> on every step.  With reduced motion it snaps.
        /// </summary>
        public static void To(Control owner, Spring spring, double target, Action onStep = null)
        {
            if (spring is null)
            {
                return;
            }
            spring.Target = target;
            if (ReducedMotion.IsOn || owner is null || owner.IsDisposed || !owner.IsHandleCreated)
            {
                spring.Snap(target);
                Repaint(owner);
                onStep?.Invoke();
                return;
            }
            if (spring.AtRest)
            {
                Repaint(owner);
                return;
            }
            foreach (Entry e in Active)
            {
                if (ReferenceEquals(e.Spring, spring))
                {
                    return; // already moving; the new target is picked up next tick
                }
            }
            Active.Add(new Entry { Owner = owner, Spring = spring, OnStep = onStep });
            Start();
        }

        private static void Start()
        {
            try
            {
                if (_timer is null)
                {
                    _timer = new Timer { Interval = 15 };
                    _timer.Tick += (s, e) => Tick();
                }
                if (!_timer.Enabled)
                {
                    _last = Clock.Elapsed.TotalSeconds;
                    _timer.Start();
                }
            }
            catch (Exception ex)
            {
                Log.Warn("animator: cannot start the timer; snapping instead", ex);
                foreach (Entry e in Active)
                {
                    e.Spring.Snap(e.Spring.Target);
                    Repaint(e.Owner);
                }
                Active.Clear();
            }
        }

        private static void Tick()
        {
            try
            {
                double now = Clock.Elapsed.TotalSeconds;
                // A stalled frame (VEGAS busy) is clamped: the spring moves on
                // from where it was instead of teleporting.
                double dt = Math.Min(now - _last, 1.0 / 20.0);
                _last = now;
                for (int i = Active.Count - 1; i >= 0; --i)
                {
                    Entry e = Active[i];
                    if (e.Owner is null || e.Owner.IsDisposed)
                    {
                        Active.RemoveAt(i);
                        continue;
                    }
                    e.Spring.Step(dt);
                    try
                    {
                        e.OnStep?.Invoke();
                    }
                    catch (Exception ex)
                    {
                        // A throwing step stops only its own spring.
                        Log.Warn("animator: a step callback threw", ex);
                        e.Spring.Snap(e.Spring.Target);
                    }
                    Repaint(e.Owner);
                    if (e.Spring.AtRest)
                    {
                        Active.RemoveAt(i);
                    }
                }
                if (Active.Count == 0)
                {
                    _timer?.Stop();
                }
            }
            catch (Exception ex)
            {
                Log.Warn("animator: a tick failed; stopping", ex);
                Active.Clear();
                _timer?.Stop();
            }
        }

        private static void Repaint(Control c)
        {
            try
            {
                if (c is not null && !c.IsDisposed)
                {
                    c.Invalidate();
                }
            }
            catch (Exception)
            {
                // Being torn down.
            }
        }
    }

    /// <summary>Windows' Settings > Accessibility > Visual effects > "Animation effects".</summary>
    internal static class ReducedMotion
    {
        private const uint SpiGetClientAreaAnimation = 0x1042;
        private static bool _cached;
        private static long _readAt = long.MinValue;

        [DllImport("user32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SystemParametersInfo(uint action, uint param, ref bool value, uint winIni);

        /// <summary>
        /// True when the user turned animation effects off
        /// (SPI_GETCLIENTAREAANIMATION = FALSE).  Re-read every two seconds, so
        /// flipping the setting takes effect without restarting VEGAS.
        /// </summary>
        public static bool IsOn
        {
            get
            {
                long now = Environment.TickCount;
                if (_readAt != long.MinValue && now - _readAt < 2000)
                {
                    return _cached;
                }
                _readAt = now;
                try
                {
                    bool animate = true;
                    _cached = SystemParametersInfo(SpiGetClientAreaAnimation, 0, ref animate, 0) && !animate;
                }
                catch (Exception)
                {
                    _cached = false;
                }
                return _cached;
            }
        }
    }
}
