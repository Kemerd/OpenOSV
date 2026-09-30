// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvControl.cs - the base of every custom-painted control in the panel.

using System;
using System.Drawing;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// Double-buffered, DPI-aware, theme-following, never-throwing-while-painting
    /// base control.
    /// </summary>
    /// <remarks>
    /// <para>
    /// WinForms' stock controls draw Windows 7 chrome that fights VEGAS's
    /// skin, so every control here paints itself.  This base supplies what
    /// they all need: a scale factor for an 8-point grid at any DPI
    /// (<see cref="S"/>), hover / press / focus state, the colour of the
    /// surface it sits on (a card or the panel), and a paint wrapper that logs
    /// instead of throwing - an exception in OnPaint would otherwise repeat on
    /// every repaint.
    /// </para>
    /// </remarks>
    internal abstract class OsvControl : Control
    {
        private Color _surface = Color.Empty;

        protected OsvControl()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                     ControlStyles.ResizeRedraw | ControlStyles.SupportsTransparentBackColor, true);
            SetStyle(ControlStyles.Selectable, false);
            TabStop = false;
            Theme.Changed += OnThemeChanged;
        }

        /// <summary>DPI scale: device pixels per 96-DPI pixel.</summary>
        public float S
        {
            get
            {
                try
                {
                    int dpi = DeviceDpi;
                    return dpi > 0 ? dpi / 96f : 1f;
                }
                catch (Exception)
                {
                    return 1f;
                }
            }
        }

        /// <summary>A length on the 8-point grid, in device pixels.</summary>
        public int Px(float points) => (int)Math.Round(points * S);

        /// <summary>The colour behind the control (card fill or panel background).</summary>
        public Color Surface
        {
            get => _surface.IsEmpty ? Theme.Current.Background : _surface;
            set
            {
                _surface = value;
                Invalidate();
            }
        }

        /// <summary>True while the pointer is over the control.</summary>
        protected bool Hovered { get; private set; }

        /// <summary>True while the left button is held on the control.</summary>
        protected bool Pressed { get; private set; }

        /// <summary>Make the control reachable with Tab (buttons, switches, segments).</summary>
        protected void MakeFocusable()
        {
            SetStyle(ControlStyles.Selectable, true);
            TabStop = true;
        }

        /// <summary>Paint the control's content.  Exceptions are logged by the caller.</summary>
        protected abstract void PaintContent(Graphics g);

        protected sealed override void OnPaint(PaintEventArgs e)
        {
            try
            {
                Draw.Prepare(e.Graphics);
                PaintContent(e.Graphics);
            }
            catch (Exception ex)
            {
                Log.Warn("ui: painting " + GetType().Name + " failed", ex);
            }
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            try
            {
                using (var b = new SolidBrush(Surface))
                {
                    e.Graphics.FillRectangle(b, ClientRectangle);
                }
            }
            catch (Exception)
            {
                // Nothing sensible to do in a background paint.
            }
        }

        protected override void OnMouseEnter(EventArgs e)
        {
            Hovered = true;
            OnVisualStateChanged();
            base.OnMouseEnter(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            Hovered = false;
            Pressed = false;
            OnVisualStateChanged();
            base.OnMouseLeave(e);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && Enabled)
            {
                Pressed = true;
                if (TabStop)
                {
                    Focus();
                }
                OnVisualStateChanged();
            }
            base.OnMouseDown(e);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            if (Pressed)
            {
                Pressed = false;
                OnVisualStateChanged();
            }
            base.OnMouseUp(e);
        }

        protected override void OnGotFocus(EventArgs e)
        {
            Invalidate();
            base.OnGotFocus(e);
        }

        protected override void OnLostFocus(EventArgs e)
        {
            Invalidate();
            base.OnLostFocus(e);
        }

        protected override void OnEnabledChanged(EventArgs e)
        {
            Invalidate();
            base.OnEnabledChanged(e);
        }

        /// <summary>Hover / press / focus changed: repaint (subclasses start springs here).</summary>
        protected virtual void OnVisualStateChanged() => Invalidate();

        /// <summary>The palette changed.</summary>
        protected virtual void OnThemeChanged() => Invalidate();

        /// <summary>True when the focus ring should show (keyboard focus only).</summary>
        protected bool ShowFocus => Focused && ShowFocusCues;

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Theme.Changed -= OnThemeChanged;
            }
            base.Dispose(disposing);
        }

        /// <summary>Raise an event handler without letting a listener's exception escape.</summary>
        protected static void Raise(EventHandler handler, object sender)
        {
            if (handler is null)
            {
                return;
            }
            try
            {
                handler(sender, EventArgs.Empty);
            }
            catch (Exception ex)
            {
                Log.Warn("ui: an event listener threw", ex);
            }
        }
    }
}
