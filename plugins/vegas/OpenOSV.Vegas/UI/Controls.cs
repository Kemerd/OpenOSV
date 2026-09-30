// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Controls.cs - buttons, segmented controls, switches, a stepper and a
// dropdown, painted to sit flush in a VEGAS dock.
//
// Each is the standard control with its standard meaning (Apple's Human
// Interface Guidelines, "Controls"): a switch for on/off, a segmented control
// for a small set of exclusive choices, a stepper for a number that moves in
// steps, a pop-up button for a longer list, one filled button for the likely
// action and tinted ones for the rest.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Globalization;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>Glyphs from Windows' own icon font (Segoe Fluent Icons, else Segoe MDL2 Assets).</summary>
    internal static class Icons
    {
        public static readonly char Add = (char)0xE710;
        public static readonly char Folder = (char)0xE8B7;
        public static readonly char Globe = (char)0xE774;
        public static readonly char Link = (char)0xE71B;
        public static readonly char Copy = (char)0xE8C8;
        public static readonly char Video = (char)0xE714;
        public static readonly char Settings = (char)0xE713;
        public static readonly char Info = (char)0xE946;
        public static readonly char Warning = (char)0xE7BA;
        public static readonly char Error = (char)0xEA39;
        public static readonly char Check = (char)0xE73E;
        public static readonly char Download = (char)0xE896;
        public static readonly char Document = (char)0xE8A5;
        public static readonly char Filter = (char)0xE71C;

        private static readonly Dictionary<float, Font> BySize = new Dictionary<float, Font>();
        private static string _family;
        private static bool _resolved;

        /// <summary>The icon font at a size (cached), or null when Windows has neither icon font.</summary>
        public static Font Font(float points)
        {
            if (!_resolved)
            {
                _resolved = true;
                foreach (string family in new[] { "Segoe Fluent Icons", "Segoe MDL2 Assets" })
                {
                    try
                    {
                        using (var f = new Font(family, 10f, FontStyle.Regular, GraphicsUnit.Point))
                        {
                            if (string.Equals(f.Name, family, StringComparison.OrdinalIgnoreCase))
                            {
                                _family = family;
                                break;
                            }
                        }
                    }
                    catch (Exception)
                    {
                        // Try the next family.
                    }
                }
            }
            if (_family is null)
            {
                return null;
            }
            if (!BySize.TryGetValue(points, out Font font))
            {
                try
                {
                    font = new Font(_family, points, FontStyle.Regular, GraphicsUnit.Point);
                }
                catch (Exception)
                {
                    return null;
                }
                BySize[points] = font;
            }
            return font;
        }

        /// <summary>Draw a glyph centred in a rectangle (nothing when there is no icon font).</summary>
        public static void Paint(Graphics g, char glyph, Rectangle bounds, Color color, float points)
        {
            Font f = Font(points);
            if (f is null || glyph == '\0')
            {
                return;
            }
            TextRenderer.DrawText(g, glyph.ToString(), f, bounds, color,
                                  TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.NoPrefix | TextFormatFlags.NoPadding);
        }
    }

    /// <summary>Button styles.</summary>
    internal enum ButtonStyle
    {
        /// <summary>Filled with the accent: the one likely action.</summary>
        Primary,
        /// <summary>Accent-tinted: secondary actions.</summary>
        Tinted,
        /// <summary>A neutral fill: tools.</summary>
        Gray,
        /// <summary>A pill that can be selected (framing looks).</summary>
        Chip,
        /// <summary>Text only (links).</summary>
        Plain,
    }

    /// <summary>A push button that dims when pressed and springs back.</summary>
    internal sealed class OsvButton : OsvControl
    {
        private readonly Spring _press = new Spring(0, 0.18);
        private bool _selected;

        public OsvButton(string text, ButtonStyle style, char glyph = '\0')
        {
            Text = text ?? string.Empty;
            Style = style;
            Glyph = glyph;
            MakeFocusable();
            Cursor = Cursors.Hand;
        }

        /// <summary>The style.</summary>
        public ButtonStyle Style { get; set; }

        /// <summary>An optional leading glyph.</summary>
        public char Glyph { get; set; }

        /// <summary>For chips: shown as chosen.</summary>
        public bool Selected
        {
            get => _selected;
            set
            {
                _selected = value;
                Invalidate();
            }
        }

        /// <summary>The height this button wants.</summary>
        public int PreferredHeight => Px(Style == ButtonStyle.Chip ? 28 : 32);

        /// <summary>The width its content wants.</summary>
        public int PreferredContentWidth
        {
            get
            {
                int text = Draw.Measure(Text, Style == ButtonStyle.Primary ? TextStyle.BodyStrong : TextStyle.Body).Width;
                int glyph = Glyph != '\0' && Icons.Font(10f) is not null ? Px(22) : 0;
                return text + glyph + Px(Style == ButtonStyle.Chip ? 24 : 32);
            }
        }

        protected override void OnVisualStateChanged()
        {
            Animator.To(this, _press, Pressed ? 1.0 : 0.0);
        }

        protected override void OnTextChanged(EventArgs e)
        {
            Invalidate();
            base.OnTextChanged(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (Enabled && (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space))
            {
                e.Handled = true;
                OnClick(EventArgs.Empty);
            }
            base.OnKeyDown(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            RectangleF r = new RectangleF(1f * s, 1f * s, Width - 2f * s, Height - 2f * s);
            float radius = Style == ButtonStyle.Chip ? r.Height / 2f : 8f * s;

            // ---- fill and text colours per style -------------------------------------
            Color fill, text;
            switch (Style)
            {
                case ButtonStyle.Primary:
                    fill = Hovered ? Theme.Mix(t.Accent, Color.White, 0.08) : t.Accent;
                    text = t.OnAccent;
                    break;
                case ButtonStyle.Tinted:
                    fill = Theme.Mix(Surface, t.Accent, Hovered ? 0.26 : 0.18);
                    text = t.Dark ? Theme.Mix(t.Accent, Color.White, 0.35) : Theme.Mix(t.Accent, Color.Black, 0.1);
                    break;
                case ButtonStyle.Chip:
                    fill = _selected ? t.Accent : (Hovered ? t.ControlHover : t.ControlFill);
                    text = _selected ? t.OnAccent : t.Text;
                    break;
                case ButtonStyle.Plain:
                    fill = Hovered ? Theme.Alpha(t.Text, 0.06) : Color.Transparent;
                    text = t.Accent;
                    break;
                default:
                    fill = Hovered ? t.ControlHover : t.ControlFill;
                    text = t.Text;
                    break;
            }
            // Pressed: dim toward the surface (the spring makes it breathe back).
            double press = _press.Value;
            if (press > 0.001)
            {
                fill = Theme.Mix(fill, Surface, 0.25 * press);
                text = Theme.Mix(text, Surface, 0.15 * press);
            }
            if (!Enabled)
            {
                fill = Theme.Mix(fill, Surface, 0.55);
                text = Theme.Mix(text, Surface, 0.55);
            }
            Draw.Fill(g, r, radius, fill);

            // ---- glyph + label, centred as a unit ---------------------------------------
            TextStyle ts = Style == ButtonStyle.Primary ? TextStyle.BodyStrong : TextStyle.Body;
            Size textSize = Draw.Measure(g, Text, ts);
            bool hasGlyph = Glyph != '\0' && Icons.Font(10f) is not null;
            int glyphW = hasGlyph ? Px(20) : 0;
            int total = textSize.Width + glyphW;
            int x = Math.Max(Px(8), (Width - total) / 2);
            if (hasGlyph)
            {
                Icons.Paint(g, Glyph, new Rectangle(x, 0, glyphW, Height), text, 10f);
            }
            Draw.Text(g, Text, ts, new Rectangle(x + glyphW, 0, Math.Max(1, Width - x - glyphW - Px(6)), Height), text);

            if (ShowFocus)
            {
                Draw.FocusRing(g, r, radius, s);
            }
        }
    }

    /// <summary>A segmented control whose selection glides between segments.</summary>
    internal sealed class OsvSegmented : OsvControl
    {
        private readonly Spring _thumb = new Spring(0, 0.3);
        private string[] _items = new string[0];
        private int _selected = -1;
        private int _hover = -1;

        public OsvSegmented(params string[] items)
        {
            Items = items;
            MakeFocusable();
            Cursor = Cursors.Hand;
        }

        /// <summary>Raised when the user picks a segment.</summary>
        public event EventHandler SelectedIndexChanged;

        /// <summary>The segment labels.</summary>
        public string[] Items
        {
            get => _items;
            set
            {
                _items = value ?? new string[0];
                Invalidate();
            }
        }

        /// <summary>The selected segment (-1 = none, e.g. a mixed selection).</summary>
        public int SelectedIndex
        {
            get => _selected;
            set => Select(value, false);
        }

        /// <summary>The height this control wants.</summary>
        public int PreferredHeight => Px(30);

        private void Select(int index, bool fromUser)
        {
            int clamped = (index >= 0 && index < _items.Length) ? index : -1;
            bool first = _selected < 0;
            if (clamped == _selected)
            {
                return;
            }
            _selected = clamped;
            if (clamped >= 0)
            {
                if (first || !fromUser)
                {
                    _thumb.Snap(clamped);
                    Invalidate();
                }
                else
                {
                    Animator.To(this, _thumb, clamped);
                }
            }
            else
            {
                Invalidate();
            }
            if (fromUser)
            {
                Raise(SelectedIndexChanged, this);
            }
        }

        private RectangleF SegmentRect(double index)
        {
            float s = S;
            float pad = 2f * s;
            float w = _items.Length == 0 ? 0 : (Width - 2 * pad) / _items.Length;
            return new RectangleF(pad + (float)index * w, pad, w, Height - 2 * pad);
        }

        private int HitTest(Point p)
        {
            if (_items.Length == 0)
            {
                return -1;
            }
            int i = (int)((p.X - 2 * S) / ((Width - 4 * S) / _items.Length));
            return (i >= 0 && i < _items.Length) ? i : -1;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            int h = HitTest(e.Location);
            if (h != _hover)
            {
                _hover = h;
                Invalidate();
            }
            base.OnMouseMove(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            _hover = -1;
            base.OnMouseLeave(e);
        }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            if (Enabled && e.Button == MouseButtons.Left)
            {
                int h = HitTest(e.Location);
                if (h >= 0)
                {
                    Select(h, true);
                }
            }
            base.OnMouseClick(e);
        }

        protected override bool IsInputKey(Keys keyData) =>
            keyData == Keys.Left || keyData == Keys.Right || base.IsInputKey(keyData);

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (Enabled && _items.Length > 0)
            {
                if (e.KeyCode == Keys.Left)
                {
                    Select(Math.Max(0, (_selected < 0 ? 0 : _selected) - 1), true);
                    e.Handled = true;
                }
                else if (e.KeyCode == Keys.Right)
                {
                    Select(Math.Min(_items.Length - 1, _selected + 1), true);
                    e.Handled = true;
                }
            }
            base.OnKeyDown(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            RectangleF track = new RectangleF(0.5f * s, 0.5f * s, Width - 1f * s, Height - 1f * s);
            Draw.Fill(g, track, 8f * s, Enabled ? t.ControlFill : Theme.Mix(t.ControlFill, Surface, 0.5));

            // ---- the thumb, where the spring has it ---------------------------------------
            if (_selected >= 0 && _items.Length > 0)
            {
                RectangleF thumb = SegmentRect(_thumb.Value);
                using (var shadow = new SolidBrush(Theme.Alpha(Color.Black, t.Dark ? 0.25 : 0.10)))
                using (GraphicsPath p = Draw.Rounded(new RectangleF(thumb.X, thumb.Y + 0.75f * s, thumb.Width, thumb.Height), 6.5f * s))
                {
                    g.FillPath(shadow, p);
                }
                Draw.Fill(g, thumb, 6.5f * s, Enabled ? t.Thumb : Theme.Mix(t.Thumb, Surface, 0.5));
            }

            // ---- separators between unselected neighbours, then labels ------------------------
            for (int i = 0; i < _items.Length; ++i)
            {
                RectangleF seg = SegmentRect(i);
                if (i > 0 && i != _selected && i - 1 != _selected)
                {
                    using (var pen = new Pen(t.Separator, 1f))
                    {
                        g.DrawLine(pen, seg.X, seg.Y + seg.Height * 0.25f, seg.X, seg.Bottom - seg.Height * 0.25f);
                    }
                }
                if (i == _hover && i != _selected && Enabled)
                {
                    Draw.Fill(g, RectangleF.Inflate(seg, -1f * s, -1f * s), 6f * s, Theme.Alpha(t.Text, 0.05));
                }
                Color text = Enabled ? (i == _selected ? t.Text : t.SecondaryText) : t.TertiaryText;
                Draw.Text(g, _items[i], i == _selected ? TextStyle.BodyStrong : TextStyle.Body, Rectangle.Round(seg), text,
                          TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            }
            if (ShowFocus)
            {
                Draw.FocusRing(g, track, 8f * s, s);
            }
        }
    }

    /// <summary>An on/off switch whose knob slides and stretches while pressed.</summary>
    internal sealed class OsvSwitch : OsvControl
    {
        private readonly Spring _knob = new Spring(0, 0.28);
        private readonly Spring _stretch = new Spring(0, 0.2);
        private bool _checked;

        public OsvSwitch()
        {
            MakeFocusable();
            Cursor = Cursors.Hand;
        }

        /// <summary>Raised when the user flips the switch.</summary>
        public event EventHandler CheckedChanged;

        /// <summary>On or off (setting it from code does not raise the event).</summary>
        public bool Checked
        {
            get => _checked;
            set
            {
                if (_checked == value)
                {
                    return;
                }
                _checked = value;
                _knob.Snap(value ? 1 : 0);
                Invalidate();
            }
        }

        /// <summary>Preferred size (51 x 31 points scaled down to the panel's density).</summary>
        public Size PreferredSwitchSize => new Size(Px(40), Px(24));

        private void Toggle()
        {
            if (!Enabled)
            {
                return;
            }
            _checked = !_checked;
            Animator.To(this, _knob, _checked ? 1 : 0);
            Raise(CheckedChanged, this);
        }

        protected override void OnVisualStateChanged()
        {
            Animator.To(this, _stretch, Pressed ? 1 : 0);
        }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                Toggle();
            }
            base.OnMouseClick(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter)
            {
                Toggle();
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            Size size = PreferredSwitchSize;
            var track = new RectangleF((Width - size.Width) / 2f, (Height - size.Height) / 2f, size.Width, size.Height);
            double k = _knob.Value;
            Color on = t.Green;
            Color off = t.ControlPressed;
            Color fill = Theme.Mix(off, on, k);
            if (!Enabled)
            {
                fill = Theme.Mix(fill, Surface, 0.5);
            }
            Draw.Fill(g, track, track.Height / 2f, fill);

            // ---- the knob: slides with the spring, widens while held --------------------------
            float pad = 2f * s;
            float d = track.Height - 2 * pad;
            float extra = (float)(_stretch.Value * 6f * s);
            float travel = track.Width - 2 * pad - d - extra;
            float x = track.X + pad + (float)(k * travel);
            var knob = new RectangleF(x, track.Y + pad, d + extra, d);
            using (var shadow = new SolidBrush(Theme.Alpha(Color.Black, 0.18)))
            using (GraphicsPath p = Draw.Rounded(new RectangleF(knob.X, knob.Y + 0.75f * s, knob.Width, knob.Height), d / 2f))
            {
                g.FillPath(shadow, p);
            }
            Draw.Fill(g, knob, d / 2f, Enabled ? Color.White : Theme.Mix(Color.White, Surface, 0.4));
            if (ShowFocus)
            {
                Draw.FocusRing(g, track, track.Height / 2f, s);
            }
        }
    }

    /// <summary>A number with - and + buttons, typed in place.</summary>
    internal sealed class OsvStepper : OsvControl
    {
        private readonly TextBox _box;
        private int _value;
        private int _hover; // -1 minus, +1 plus, 0 none

        public OsvStepper()
        {
            _box = new TextBox
            {
                BorderStyle = BorderStyle.None,
                TextAlign = HorizontalAlignment.Center,
                Font = Typeface.Of(TextStyle.Body),
            };
            _box.KeyDown += (s, e) =>
            {
                if (e.KeyCode == Keys.Enter)
                {
                    Commit();
                    e.SuppressKeyPress = true;
                }
                else if (e.KeyCode == Keys.Up)
                {
                    Step(+1);
                    e.SuppressKeyPress = true;
                }
                else if (e.KeyCode == Keys.Down)
                {
                    Step(-1);
                    e.SuppressKeyPress = true;
                }
            };
            _box.LostFocus += (s, e) => Commit();
            Controls.Add(_box);
            Cursor = Cursors.Hand;
            ApplyColours();
            ShowValue();
        }

        /// <summary>Raised when the value is committed by the user.</summary>
        public event EventHandler ValueChanged;

        /// <summary>Smallest value.</summary>
        public int Minimum { get; set; }

        /// <summary>Largest value.</summary>
        public int Maximum { get; set; } = 10_000_000;

        /// <summary>The value (setting it from code does not raise the event).</summary>
        public int Value
        {
            get => _value;
            set
            {
                _value = Clamp(value);
                ShowValue();
            }
        }

        /// <summary>The height this control wants.</summary>
        public int PreferredHeight => Px(30);

        private int Clamp(int v) => v < Minimum ? Minimum : (v > Maximum ? Maximum : v);

        private void ShowValue()
        {
            string text = _value.ToString(CultureInfo.CurrentCulture);
            if (_box.Text != text)
            {
                _box.Text = text;
            }
        }

        private void Step(int delta)
        {
            if (!Enabled)
            {
                return;
            }
            int next = Clamp(_value + delta);
            if (next == _value)
            {
                return;
            }
            _value = next;
            ShowValue();
            Raise(ValueChanged, this);
        }

        private void Commit()
        {
            if (!Enabled)
            {
                return;
            }
            if (int.TryParse(_box.Text.Trim(), NumberStyles.Integer, CultureInfo.CurrentCulture, out int typed))
            {
                typed = Clamp(typed);
                if (typed != _value)
                {
                    _value = typed;
                    ShowValue();
                    Raise(ValueChanged, this);
                    return;
                }
            }
            ShowValue();
        }

        private void ApplyColours()
        {
            Theme t = Theme.Current;
            _box.BackColor = t.ControlFill;
            _box.ForeColor = Enabled ? t.Text : t.TertiaryText;
        }

        protected override void OnThemeChanged()
        {
            ApplyColours();
            base.OnThemeChanged();
        }

        protected override void OnEnabledChanged(EventArgs e)
        {
            _box.Enabled = Enabled;
            ApplyColours();
            base.OnEnabledChanged(e);
        }

        protected override void OnLayout(LayoutEventArgs e)
        {
            base.OnLayout(e);
            int button = Height;
            int boxHeight = _box.PreferredHeight;
            _box.SetBounds(button + Px(4), Math.Max(0, (Height - boxHeight) / 2), Math.Max(10, Width - 2 * button - Px(8)), boxHeight);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            int h = e.X < Height ? -1 : (e.X > Width - Height ? 1 : 0);
            if (h != _hover)
            {
                _hover = h;
                Invalidate();
            }
            base.OnMouseMove(e);
        }

        protected override void OnMouseLeave(EventArgs e)
        {
            _hover = 0;
            base.OnMouseLeave(e);
        }

        protected override void OnMouseClick(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                if (e.X < Height)
                {
                    Step(-1);
                }
                else if (e.X > Width - Height)
                {
                    Step(+1);
                }
            }
            base.OnMouseClick(e);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            // The wheel changes the number only while the stepper has focus:
            // scrolling the panel past it must never edit a clip.
            if (ContainsFocus)
            {
                Step(e.Delta > 0 ? 1 : -1);
                if (e is HandledMouseEventArgs handled)
                {
                    handled.Handled = true;
                }
            }
            base.OnMouseWheel(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            var r = new RectangleF(0.5f * s, 0.5f * s, Width - 1f * s, Height - 1f * s);
            Draw.Fill(g, r, 8f * s, t.ControlFill);
            int b = Height;
            if (_hover != 0 && Enabled)
            {
                var hot = _hover < 0 ? new RectangleF(r.X, r.Y, b, r.Height) : new RectangleF(r.Right - b, r.Y, b, r.Height);
                Draw.Fill(g, hot, 8f * s, t.ControlHover);
            }
            Color glyph = Enabled ? t.Text : t.TertiaryText;
            using (var pen = new Pen(glyph, 1.6f * s) { StartCap = LineCap.Round, EndCap = LineCap.Round })
            {
                float cy = Height / 2f;
                float half = 4.5f * s;
                float mx = b / 2f;
                float px = Width - b / 2f;
                g.DrawLine(pen, mx - half, cy, mx + half, cy);
                g.DrawLine(pen, px - half, cy, px + half, cy);
                g.DrawLine(pen, px, cy - half, px, cy + half);
            }
        }
    }

    /// <summary>A pop-up button: the current choice and a chevron; click for the list.</summary>
    internal sealed class OsvDropdown : OsvControl
    {
        private string[] _items = new string[0];
        private int _selected = -1;

        public OsvDropdown(params string[] items)
        {
            Items = items;
            MakeFocusable();
            Cursor = Cursors.Hand;
        }

        /// <summary>Raised when the user picks an item.</summary>
        public event EventHandler SelectedIndexChanged;

        /// <summary>The items.</summary>
        public string[] Items
        {
            get => _items;
            set
            {
                _items = value ?? new string[0];
                Invalidate();
            }
        }

        /// <summary>The chosen item (-1 = none / mixed).</summary>
        public int SelectedIndex
        {
            get => _selected;
            set
            {
                _selected = (value >= 0 && value < _items.Length) ? value : -1;
                Invalidate();
            }
        }

        /// <summary>Placeholder when nothing is chosen.</summary>
        public string Placeholder { get; set; } = "Choose";

        /// <summary>The height this control wants.</summary>
        public int PreferredHeight => Px(30);

        protected override void OnMouseClick(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left)
            {
                Open();
            }
            base.OnMouseClick(e);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Space || e.KeyCode == Keys.Enter || e.KeyCode == Keys.Down)
            {
                Open();
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        private void Open()
        {
            if (!Enabled || _items.Length == 0)
            {
                return;
            }
            try
            {
                var menu = new ContextMenuStrip { Renderer = new MenuRenderer(), ShowCheckMargin = true, ShowImageMargin = false, Font = Typeface.Of(TextStyle.Body) };
                for (int i = 0; i < _items.Length; ++i)
                {
                    int index = i;
                    var item = new ToolStripMenuItem(_items[i]) { Checked = i == _selected };
                    item.Click += (s, e) =>
                    {
                        if (index != _selected)
                        {
                            _selected = index;
                            Invalidate();
                            Raise(SelectedIndexChanged, this);
                        }
                    };
                    menu.Items.Add(item);
                }
                menu.Closed += (s, e) => BeginInvoke(new Action(menu.Dispose));
                menu.Show(this, new Point(0, Height + Px(2)));
            }
            catch (Exception ex)
            {
                Log.Warn("ui: opening a dropdown failed", ex);
            }
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            var r = new RectangleF(0.5f * s, 0.5f * s, Width - 1f * s, Height - 1f * s);
            Draw.Fill(g, r, 8f * s, Enabled ? (Hovered ? t.ControlHover : t.ControlFill) : Theme.Mix(t.ControlFill, Surface, 0.5));
            string text = _selected >= 0 ? _items[_selected] : Placeholder;
            Color color = !Enabled ? t.TertiaryText : (_selected >= 0 ? t.Text : t.SecondaryText);
            Draw.Text(g, text, TextStyle.Body, new Rectangle(Px(10), 0, Math.Max(1, Width - Px(34)), Height), color);
            Draw.Chevron(g, new PointF(Width - Px(16), Height / 2f), 3.5f * s, 0f, Enabled ? t.SecondaryText : t.TertiaryText, 1.5f * s);
            if (ShowFocus)
            {
                Draw.FocusRing(g, r, 8f * s, s);
            }
        }

        /// <summary>A menu painted in the panel's palette instead of Windows' white.</summary>
        private sealed class MenuRenderer : ToolStripProfessionalRenderer
        {
            public MenuRenderer() : base(new MenuColours())
            {
                RoundedEdges = true;
            }

            protected override void OnRenderItemText(ToolStripItemTextRenderEventArgs e)
            {
                e.TextColor = e.Item.Selected ? Theme.Current.OnAccent : Theme.Current.Text;
                base.OnRenderItemText(e);
            }

            protected override void OnRenderItemCheck(ToolStripItemImageRenderEventArgs e)
            {
                try
                {
                    Rectangle r = e.ImageRectangle;
                    Icons.Paint(e.Graphics, Icons.Check, r, e.Item.Selected ? Theme.Current.OnAccent : Theme.Current.Accent, 9f);
                }
                catch (Exception)
                {
                    base.OnRenderItemCheck(e);
                }
            }
        }

        private sealed class MenuColours : ProfessionalColorTable
        {
            private static Theme T => Theme.Current;
            public override Color ToolStripDropDownBackground => Theme.Mix(T.Card, T.Text, 0.04);
            public override Color ImageMarginGradientBegin => ToolStripDropDownBackground;
            public override Color ImageMarginGradientMiddle => ToolStripDropDownBackground;
            public override Color ImageMarginGradientEnd => ToolStripDropDownBackground;
            public override Color MenuBorder => T.CardBorder;
            public override Color MenuItemBorder => T.Accent;
            public override Color MenuItemSelected => T.Accent;
            public override Color MenuItemSelectedGradientBegin => T.Accent;
            public override Color MenuItemSelectedGradientEnd => T.Accent;
            public override Color CheckBackground => Color.Transparent;
            public override Color CheckSelectedBackground => Color.Transparent;
            public override Color CheckPressedBackground => Color.Transparent;
            public override Color SeparatorDark => T.Separator;
            public override Color SeparatorLight => T.Separator;
        }
    }
}
