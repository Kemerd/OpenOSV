// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Surfaces.cs - inset grouped cards, the banner for missing pieces, and the
// status line.

using System;
using System.Drawing;
using System.Globalization;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// An inset grouped card: a small section header above a rounded, lifted
    /// surface holding controls (Apple's grouped lists).  Optionally
    /// collapsible, with a chevron that turns and a height that springs.
    /// </summary>
    internal sealed class Card : Panel
    {
        private readonly Spring _open = new Spring(1, 0.34);
        private string _title;
        private bool _expanded = true;
        private bool _hoverHeader;

        public Card(string title, bool collapsible = false)
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            _title = title ?? string.Empty;
            Collapsible = collapsible;
            Theme.Changed += OnThemeChanged;
        }

        /// <summary>Raised when a collapsible card is opened or folded.</summary>
        public event EventHandler ExpandedChanged;

        /// <summary>Raised on every animation step of a fold (the owner re-lays out).</summary>
        public event EventHandler HeightAnimating;

        /// <summary>Lays out the children in content coordinates; returns the content height.</summary>
        public Func<int, int> Arrange { get; set; }

        /// <summary>The header text.</summary>
        public string Title
        {
            get => _title;
            set
            {
                _title = value ?? string.Empty;
                Invalidate();
            }
        }

        /// <summary>True when a click on the header folds the card.</summary>
        public bool Collapsible { get; }

        /// <summary>Open (true) or folded.</summary>
        public bool Expanded
        {
            get => _expanded;
            set => SetExpanded(value, false);
        }

        private float S => DeviceDpi > 0 ? DeviceDpi / 96f : 1f;
        private int Px(float v) => (int)Math.Round(v * S);

        /// <summary>Height of the header strip above the card surface.</summary>
        public int HeaderHeight => string.IsNullOrEmpty(_title) ? 0 : Px(26);

        /// <summary>Inner padding of the surface.</summary>
        public int Inner => Px(12);

        /// <summary>
        /// Lay out for <paramref name="width"/> and return the card's height
        /// (header + surface), following the fold spring: the surface's height
        /// is its content's scaled by how open the card is, and the controls
        /// hide once it is (nearly) shut.
        /// </summary>
        public int LayoutFor(int width)
        {
            int content = 0;
            try
            {
                content = Arrange?.Invoke(Math.Max(10, width - 2 * Inner)) ?? 0;
            }
            catch (Exception ex)
            {
                Log.Warn("ui: laying out the '" + _title + "' card failed", ex);
            }
            double open = Collapsible ? _open.Value : 1.0;
            int full = content + 2 * Inner;
            int body = (int)Math.Round(full * open);
            bool showChildren = open > 0.02;
            foreach (Control c in Controls)
            {
                if (!showChildren)
                {
                    c.Visible = false;
                }
            }
            return HeaderHeight + body;
        }

        /// <summary>Offset of the content area inside the card.</summary>
        public Point ContentOrigin => new Point(Inner, HeaderHeight + Inner);

        /// <summary>Place a child in CONTENT coordinates (the card adds its header and padding).</summary>
        public void Place(Control c, int x, int y, int width, int height, bool visible = true)
        {
            if (c is null)
            {
                return;
            }
            c.SetBounds(Inner + x, HeaderHeight + Inner + y, Math.Max(1, width), Math.Max(1, height));
            c.Visible = visible;
        }

        private void SetExpanded(bool value, bool animate)
        {
            if (_expanded == value)
            {
                return;
            }
            _expanded = value;
            if (animate)
            {
                Animator.To(this, _open, value ? 1 : 0, () => HeightAnimating?.Invoke(this, EventArgs.Empty));
            }
            else
            {
                _open.Snap(value ? 1 : 0);
            }
            try
            {
                ExpandedChanged?.Invoke(this, EventArgs.Empty);
            }
            catch (Exception ex)
            {
                Log.Warn("ui: a card listener threw", ex);
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            bool hover = Collapsible && e.Y < HeaderHeight;
            if (hover != _hoverHeader)
            {
                _hoverHeader = hover;
                Cursor = hover ? Cursors.Hand : Cursors.Default;
                Invalidate(new Rectangle(0, 0, Width, HeaderHeight));
            }
            base.OnMouseMove(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            _hoverHeader = false;
            Invalidate();
            base.OnMouseLeave(e);
        }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            if (Collapsible && e.Y < HeaderHeight && e.Button == MouseButtons.Left)
            {
                SetExpanded(!_expanded, true);
            }
            base.OnMouseClick(e);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            using (var b = new SolidBrush(Theme.Current.Background))
            {
                e.Graphics.FillRectangle(b, ClientRectangle);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            try
            {
                Graphics g = e.Graphics;
                Draw.Prepare(g);
                Theme t = Theme.Current;
                float s = S;
                // ---- header: small, secondary, with a turning chevron when foldable -----
                if (HeaderHeight > 0)
                {
                    int textLeft = Px(4);
                    if (Collapsible)
                    {
                        float angle = (float)(-90.0 + 90.0 * _open.Value);
                        Draw.Chevron(g, new PointF(Px(8), HeaderHeight / 2f), 3.2f * s, angle, _hoverHeader ? t.Text : t.SecondaryText, 1.5f * s);
                        textLeft = Px(18);
                    }
                    Draw.Text(g, _title.ToUpper(CultureInfo.CurrentCulture), TextStyle.Section,
                              new Rectangle(textLeft, 0, Width - textLeft, HeaderHeight), _hoverHeader ? t.Text : t.SecondaryText);
                }
                // ---- the surface ---------------------------------------------------------------------
                int bodyHeight = Height - HeaderHeight;
                if (bodyHeight > 1)
                {
                    var body = new RectangleF(0.5f * s, HeaderHeight + 0.5f * s, Width - 1f * s, bodyHeight - 1f * s);
                    Draw.Fill(g, body, 12f * s, t.Card);
                    Draw.Stroke(g, body, 12f * s, t.CardBorder, 1f);
                }
            }
            catch (Exception ex)
            {
                Log.Warn("ui: painting a card failed", ex);
            }
        }

        private void OnThemeChanged()
        {
            foreach (Control c in Controls)
            {
                if (c is OsvControl oc)
                {
                    oc.Surface = Theme.Current.Card;
                }
            }
            Invalidate();
        }

        /// <summary>Add a control that sits on the card's surface.</summary>
        public T Add<T>(T control) where T : Control
        {
            if (control is OsvControl oc)
            {
                oc.Surface = Theme.Current.Card;
            }
            Controls.Add(control);
            return control;
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Theme.Changed -= OnThemeChanged;
            }
            base.Dispose(disposing);
        }
    }

    /// <summary>What a line of text is saying, for its colour.</summary>
    internal enum TextTone
    {
        /// <summary>Primary or secondary text.</summary>
        Default,
        /// <summary>Something to check (orange).</summary>
        Warning,
        /// <summary>Something failed (red).</summary>
        Error,
    }

    /// <summary>A line of wrapped text painted on a card (captions, help, the clip header).</summary>
    internal sealed class TextBlock : OsvControl
    {
        private string _text = string.Empty;

        public TextBlock(TextStyle style, bool secondary = true)
        {
            Style = style;
            Secondary = secondary;
        }

        /// <summary>The type style.</summary>
        public TextStyle Style { get; set; }

        /// <summary>Secondary colour (true) or primary.</summary>
        public bool Secondary { get; set; }

        /// <summary>Override colour (Empty = by <see cref="Secondary"/>).</summary>
        public Color Colour { get; set; } = Color.Empty;

        /// <summary>A semantic tone, resolved against the palette at paint time (follows theme changes).</summary>
        public TextTone Tone
        {
            get => _tone;
            set
            {
                _tone = value;
                Invalidate();
            }
        }

        private TextTone _tone = TextTone.Default;

        /// <summary>
        /// One line that shortens in the MIDDLE when it has to ("E:\DCIM\...\CAM_0010.OSV"),
        /// for paths, instead of wrapping where a path has no spaces.
        /// </summary>
        public bool PathLine { get; set; }

        /// <summary>The text.</summary>
        public string Content
        {
            get => _text;
            set
            {
                _text = value ?? string.Empty;
                Invalidate();
            }
        }

        /// <summary>The height the text needs at a width.</summary>
        public int HeightFor(int width)
        {
            if (string.IsNullOrEmpty(_text))
            {
                return 0;
            }
            return (PathLine ? Draw.Measure(_text, Style).Height : Draw.Measure(_text, Style, width, true).Height) + Px(2);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            Color c;
            switch (_tone)
            {
                case TextTone.Warning: c = t.WarningText; break;
                case TextTone.Error: c = t.ErrorText; break;
                default: c = !Colour.IsEmpty ? Colour : (Secondary ? t.SecondaryText : t.Text); break;
            }
            TextFormatFlags flags = PathLine
                ? TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.SingleLine | TextFormatFlags.PathEllipsis
                : TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.WordBreak;
            Draw.Text(g, _text, Style, ClientRectangle, c, flags);
        }
    }

    /// <summary>
    /// A tinted notice for something missing (the OpenFX bundle, osvtool)
    /// with one button that fixes it or explains how.
    /// </summary>
    internal sealed class Banner : Panel
    {
        private string _message = string.Empty;

        public Banner()
        {
            SetStyle(ControlStyles.UserPaint | ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
            Action = new OsvButton("Fix", ButtonStyle.Tinted);
            Controls.Add(Action);
            Theme.Changed += Invalidate;
        }

        /// <summary>The banner's button (hidden when it has no text).</summary>
        public OsvButton Action { get; }

        /// <summary>Warning (orange) or error (red).</summary>
        public StatusKind Kind { get; set; } = StatusKind.Warning;

        /// <summary>The message.</summary>
        public string Message
        {
            get => _message;
            set
            {
                _message = value ?? string.Empty;
                Invalidate();
            }
        }

        private float S => DeviceDpi > 0 ? DeviceDpi / 96f : 1f;
        private int Px(float v) => (int)Math.Round(v * S);

        /// <summary>Lay out for a width; returns the height.</summary>
        public int LayoutFor(int width)
        {
            int textLeft = Px(40);
            int textWidth = Math.Max(20, width - textLeft - Px(12));
            int textH = Draw.Measure(_message, TextStyle.Caption, textWidth, true).Height;
            int y = Px(10) + textH + Px(8);
            bool hasButton = !string.IsNullOrEmpty(Action.Text);
            Action.Visible = hasButton;
            if (hasButton)
            {
                int bw = Math.Min(width - textLeft - Px(12), Math.Max(Px(96), Action.PreferredContentWidth));
                Action.SetBounds(textLeft, y, bw, Action.PreferredHeight);
                Action.Surface = Fill;
                y += Action.PreferredHeight + Px(10);
            }
            return Math.Max(Px(44), y);
        }

        private Color Tint => Kind == StatusKind.Error ? Theme.Current.Red : Theme.Current.Orange;

        private Color Fill => Theme.Mix(Theme.Current.Background, Tint, 0.14);

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            using (var b = new SolidBrush(Theme.Current.Background))
            {
                e.Graphics.FillRectangle(b, ClientRectangle);
            }
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            try
            {
                Graphics g = e.Graphics;
                Draw.Prepare(g);
                float s = S;
                var r = new RectangleF(0.5f * s, 0.5f * s, Width - 1f * s, Height - 1f * s);
                Draw.Fill(g, r, 12f * s, Fill);
                Draw.Stroke(g, r, 12f * s, Theme.Alpha(Tint, 0.45), 1f);
                Icons.Paint(g, Kind == StatusKind.Error ? Icons.Error : Icons.Warning, new Rectangle(Px(10), Px(10), Px(22), Px(22)), Tint, 12f);
                int textLeft = Px(40);
                Draw.Text(g, _message, TextStyle.Caption, new Rectangle(textLeft, Px(10), Width - textLeft - Px(12), Height), Theme.Current.Text,
                          TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.WordBreak);
            }
            catch (Exception ex)
            {
                Log.Warn("ui: painting a banner failed", ex);
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Theme.Changed -= Invalidate;
            }
            base.Dispose(disposing);
        }
    }

    /// <summary>
    /// The panel's last word: what just happened, when, and how it went.  A
    /// new line rises into place while the old one lifts away.
    /// </summary>
    internal sealed class StatusBar : OsvControl
    {
        private readonly Spring _rise = new Spring(1, 0.45);
        private string _text = "Ready.";
        private string _previous = string.Empty;
        private StatusKind _kind = StatusKind.Info;
        private DateTime _at = DateTime.Now;

        /// <summary>The height this bar wants.</summary>
        public int PreferredHeight => Px(30);

        /// <summary>Show a new status line.</summary>
        public void Show(string text, StatusKind kind)
        {
            _previous = _text;
            _text = string.IsNullOrWhiteSpace(text) ? string.Empty : text.Replace('\n', ' ');
            _kind = kind;
            _at = DateTime.Now;
            _rise.Snap(0);
            Animator.To(this, _rise, 1);
            Invalidate();
        }

        /// <summary>The full text, for the tooltip.</summary>
        public string FullText => _text;

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            using (var pen = new Pen(t.Separator, 1f))
            {
                g.DrawLine(pen, 0, 0.5f, Width, 0.5f);
            }
            Color dot;
            switch (_kind)
            {
                case StatusKind.Success: dot = t.Green; break;
                case StatusKind.Warning: dot = t.Orange; break;
                case StatusKind.Error: dot = t.Red; break;
                default: dot = t.Accent; break;
            }
            float d = 7f * s;
            using (var b = new SolidBrush(dot))
            {
                g.FillEllipse(b, Px(12), (Height - d) / 2f, d, d);
            }

            string stamp = _at.ToString("t", CultureInfo.CurrentCulture);
            Size stampSize = Draw.Measure(g, stamp, TextStyle.Micro);
            int stampLeft = Width - stampSize.Width - Px(12);
            Draw.Text(g, stamp, TextStyle.Micro, new Rectangle(stampLeft, 0, stampSize.Width + Px(4), Height), t.TertiaryText);

            // ---- the old line lifts away, the new one rises ---------------------------------
            double p = _rise.Value;
            int left = Px(26);
            int width = Math.Max(10, stampLeft - left - Px(8));
            int travel = Px(10);
            if (p < 0.999 && !string.IsNullOrEmpty(_previous))
            {
                var oldRect = new Rectangle(left, (int)(-travel * p), width, Height);
                Draw.Text(g, _previous, TextStyle.Caption, oldRect, Theme.Mix(t.SecondaryText, Surface, p));
            }
            var newRect = new Rectangle(left, (int)(travel * (1 - p)), width, Height);
            Draw.Text(g, _text, TextStyle.Caption, newRect, Theme.Mix(Surface, t.Text, p));
        }
    }
}
