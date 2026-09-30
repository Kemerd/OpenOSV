// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ScrollHost.cs - a vertical scroller with an overlay thumb, in place of
// WinForms' native scroll bar (which stays light grey in VEGAS's dark skin).

using System;
using System.Drawing;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// Scrolls one tall content control inside itself.
    /// </summary>
    /// <remarks>
    /// <para>
    /// The thumb is an overlay in the right margin: thin while resting, it
    /// fades in while the content moves and widens under the pointer, like the
    /// platform's overlay scrollers - so the cards keep the panel's full width
    /// and nothing jumps when the content outgrows the panel.
    /// </para>
    /// <para>
    /// Wheel and touch-pad scrolling glide on a critically damped spring
    /// (snapping when Windows' animation effects are off).  A child that does
    /// not use the wheel lets Windows pass WM_MOUSEWHEEL up to its parent, so
    /// the whole panel scrolls wherever the pointer is.  Tabbing to a control
    /// below the fold scrolls it into view.
    /// </para>
    /// </remarks>
    internal sealed class ScrollHost : Control
    {
        private const int WmMouseWheel = 0x020A;

        private readonly Control _content;
        private readonly ScrollThumb _thumb;
        private readonly Spring _offset = new Spring(0, 0.22);
        private readonly Timer _fade;

        public ScrollHost(Control content)
        {
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint | ControlStyles.UserPaint, true);
            _content = content ?? throw new ArgumentNullException(nameof(content));
            _thumb = new ScrollThumb(this);
            Controls.Add(_content);
            Controls.Add(_thumb);
            _thumb.BringToFront();
            _fade = new Timer { Interval = 900 };
            _fade.Tick += (s, e) =>
            {
                _fade.Stop();
                _thumb.Show(false);
            };
            // A control reached with Tab is scrolled into view.
            _content.ControlAdded += (s, e) => Watch(e.Control);
        }

        /// <summary>How far the content is scrolled, in pixels.</summary>
        public int Offset => (int)Math.Round(_offset.Value);

        /// <summary>The furthest the content can scroll.</summary>
        public int MaxOffset => Math.Max(0, _content.Height - ClientSize.Height);

        /// <summary>The fraction of the content that is visible (1 = no scrolling).</summary>
        internal double VisibleFraction => _content.Height <= 0 ? 1.0 : Math.Min(1.0, (double)ClientSize.Height / _content.Height);

        /// <summary>The scroll position as a fraction of the way down (0..1).</summary>
        internal double Position => MaxOffset <= 0 ? 0.0 : _offset.Value / MaxOffset;

        /// <summary>Call after the content changed height: clamps the offset and moves the content.</summary>
        public void UpdateExtent()
        {
            double clamped = Math.Max(0, Math.Min(MaxOffset, _offset.Target));
            if (Math.Abs(clamped - _offset.Target) > 0.5)
            {
                _offset.Snap(clamped);
            }
            Place();
        }

        /// <summary>Scroll to an offset (animated unless reduced motion is on).</summary>
        public void ScrollTo(double offset, bool animate = true)
        {
            double target = Math.Max(0, Math.Min(MaxOffset, offset));
            if (!animate)
            {
                _offset.Snap(target);
                Place();
                return;
            }
            Animator.To(this, _offset, target, Place);
            if (ReducedMotion.IsOn)
            {
                Place();
            }
            Flash();
        }

        /// <summary>Show the thumb for a moment.</summary>
        internal void Flash()
        {
            if (MaxOffset <= 0)
            {
                return;
            }
            _thumb.Show(true);
            _fade.Stop();
            _fade.Start();
        }

        private void Place()
        {
            try
            {
                _content.Location = new Point(0, -Offset);
                _thumb.SetBounds(ClientSize.Width - _thumb.Width, 0, _thumb.Width, ClientSize.Height);
                _thumb.Invalidate();
            }
            catch (Exception ex)
            {
                Log.Debug("scroll: placing the content failed: " + ex.Message);
            }
        }

        protected override void OnResize(EventArgs e)
        {
            base.OnResize(e);
            _thumb.Width = (int)Math.Round(10 * (DeviceDpi > 0 ? DeviceDpi / 96f : 1f));
            UpdateExtent();
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            using (var b = new SolidBrush(Theme.Current.Background))
            {
                e.Graphics.FillRectangle(b, ClientRectangle);
            }
        }

        protected override void WndProc(ref Message m)
        {
            // Wheel messages arrive here from any child that did not use them.
            if (m.Msg == WmMouseWheel)
            {
                try
                {
                    int delta = unchecked((short)((long)m.WParam >> 16));
                    Wheel(delta);
                    m.Result = IntPtr.Zero;
                    return;
                }
                catch (Exception ex)
                {
                    Log.Debug("scroll: a wheel message failed: " + ex.Message);
                }
            }
            base.WndProc(ref m);
        }

        /// <summary>One wheel notch (120) moves three lines of 24 points.</summary>
        internal void Wheel(int delta)
        {
            if (MaxOffset <= 0)
            {
                return;
            }
            double step = 72.0 * (DeviceDpi > 0 ? DeviceDpi / 96.0 : 1.0) * delta / 120.0;
            ScrollTo(_offset.Target - step);
        }

        /// <summary>Scroll so a focused control is fully visible.</summary>
        private void Watch(Control c)
        {
            if (c is null)
            {
                return;
            }
            c.Enter += (s, e) => Guard(() => Reveal(c));
            foreach (Control child in c.Controls)
            {
                Watch(child);
            }
            c.ControlAdded += (s, e) => Watch(e.Control);
        }

        private void Reveal(Control c)
        {
            Rectangle r = _content.RectangleToClient(c.RectangleToScreen(c.ClientRectangle));
            int top = r.Top;
            int bottom = r.Bottom;
            int view = ClientSize.Height;
            double target = _offset.Target;
            if (top < target)
            {
                target = top - 8;
            }
            else if (bottom > target + view)
            {
                target = bottom - view + 8;
            }
            if (Math.Abs(target - _offset.Target) > 0.5)
            {
                ScrollTo(target);
            }
        }

        private static void Guard(Action a)
        {
            try
            {
                a();
            }
            catch (Exception ex)
            {
                Log.Debug("scroll: " + ex.Message);
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _fade.Dispose();
            }
            base.Dispose(disposing);
        }

        /// <summary>The overlay thumb: fades in while scrolling, widens under the pointer, can be dragged.</summary>
        private sealed class ScrollThumb : OsvControl
        {
            private readonly ScrollHost _host;
            private readonly Spring _alpha = new Spring(0, 0.25);
            private readonly Spring _width = new Spring(0, 0.2);
            private int _dragFrom = -1;
            private double _dragOffset;

            public ScrollThumb(ScrollHost host)
            {
                _host = host;
                Cursor = Cursors.Default;
            }

            public void Show(bool visible)
            {
                Animator.To(this, _alpha, visible || Hovered || _dragFrom >= 0 ? 1 : 0);
            }

            protected override void OnVisualStateChanged()
            {
                Animator.To(this, _width, Hovered || _dragFrom >= 0 ? 1 : 0);
                Show(Hovered);
                if (!Hovered && _dragFrom < 0)
                {
                    _host.Flash();
                }
            }

            private RectangleF ThumbRect()
            {
                double visible = _host.VisibleFraction;
                float h = Math.Max(Px(28), (float)(Height * visible));
                float y = (float)((Height - h) * _host.Position);
                float w = (float)(Px(4) + _width.Value * Px(3));
                return new RectangleF(Width - w - Px(2), y + Px(2), w, h - Px(4));
            }

            protected override void OnMouseDown(MouseEventArgs e)
            {
                base.OnMouseDown(e);
                if (e.Button != MouseButtons.Left || _host.MaxOffset <= 0)
                {
                    return;
                }
                RectangleF thumb = ThumbRect();
                if (e.Y < thumb.Top || e.Y > thumb.Bottom)
                {
                    // A click on the track pages toward it.
                    _host.ScrollTo(_host.Offset + Math.Sign(e.Y - thumb.Top) * _host.ClientSize.Height * 0.9);
                    return;
                }
                _dragFrom = e.Y;
                _dragOffset = _host.Offset;
                Capture = true;
            }

            protected override void OnMouseMove(MouseEventArgs e)
            {
                base.OnMouseMove(e);
                if (_dragFrom < 0)
                {
                    return;
                }
                RectangleF thumb = ThumbRect();
                double travel = Math.Max(1.0, Height - thumb.Height);
                double offset = _dragOffset + (e.Y - _dragFrom) / travel * _host.MaxOffset;
                _host.ScrollTo(offset, false);
                Invalidate();
            }

            protected override void OnMouseUp(MouseEventArgs e)
            {
                base.OnMouseUp(e);
                _dragFrom = -1;
                Capture = false;
                OnVisualStateChanged();
            }

            protected override void PaintContent(Graphics g)
            {
                if (_host.MaxOffset <= 0 || _alpha.Value < 0.01)
                {
                    return;
                }
                Theme t = Theme.Current;
                RectangleF thumb = ThumbRect();
                Color ink = Theme.Mix(t.Background, t.Text, (0.28 + 0.17 * _width.Value) * _alpha.Value);
                Draw.Fill(g, thumb, thumb.Width / 2f, ink);
            }
        }
    }
}
