// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ClipList.cs - the project's OpenOSV clips, one row each.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>One row of the clip list (a snapshot; the list never holds VEGAS objects).</summary>
    internal sealed class ClipRow
    {
        /// <summary>The media's pool key (the row's identity).</summary>
        public string Key { get; set; }

        /// <summary>The clip's file name.</summary>
        public string Name { get; set; }

        /// <summary>"360 equirect - 3840 x 1920 - 0:12.34".</summary>
        public string Detail { get; set; }

        /// <summary>Playing the .LRF proxy.</summary>
        public bool Proxy { get; set; }

        /// <summary>The file is missing.</summary>
        public bool Offline { get; set; }

        /// <summary>Output is 360 equirect.</summary>
        public bool Equirect { get; set; }

        /// <summary>How many timeline events play it.</summary>
        public int EventCount { get; set; }
    }

    /// <summary>
    /// A list of clips: name, what it outputs and how long it is, and badges
    /// for "LRF" (proxy), "Offline" and the number of events sharing it.
    /// The selection highlight glides between rows.
    /// </summary>
    internal sealed class ClipList : OsvControl
    {
        /// <summary>Rows shown before the list scrolls.</summary>
        public const int VisibleRows = 5;

        private readonly Spring _highlight = new Spring(0, 0.3);
        private List<ClipRow> _rows = new List<ClipRow>();
        private string _selectedKey;
        private int _hover = -1;
        private int _scroll;

        public ClipList()
        {
            MakeFocusable();
        }

        /// <summary>Raised when the user selects a row.</summary>
        public event EventHandler SelectionChanged;

        /// <summary>Row height.</summary>
        public int RowHeight => Px(46);

        /// <summary>The height the list wants for its rows (at least one row for the empty state).</summary>
        public int PreferredHeight => Math.Max(1, Math.Min(VisibleRows, Math.Max(1, _rows.Count))) * RowHeight;

        /// <summary>The selected row's key, or null.</summary>
        public string SelectedKey => _selectedKey;

        /// <summary>The selected row, or null.</summary>
        public ClipRow SelectedRow => _rows.Find(r => r.Key == _selectedKey);

        /// <summary>The rows (keeps the selection when its key survives).</summary>
        public void SetRows(List<ClipRow> rows)
        {
            _rows = rows ?? new List<ClipRow>();
            int index = IndexOf(_selectedKey);
            if (index < 0)
            {
                _selectedKey = _rows.Count > 0 ? _rows[0].Key : null;
                index = _rows.Count > 0 ? 0 : -1;
                _highlight.Snap(Math.Max(0, index));
                Raise(SelectionChanged, this);
            }
            ClampScroll();
            Invalidate();
        }

        /// <summary>Select a row by key from code (a timeline selection); no event when unchanged.</summary>
        public void SelectKey(string key)
        {
            int index = IndexOf(key);
            if (index < 0 || key == _selectedKey)
            {
                return;
            }
            SelectIndex(index, true);
        }

        private int IndexOf(string key)
        {
            if (key is null)
            {
                return -1;
            }
            for (int i = 0; i < _rows.Count; ++i)
            {
                if (_rows[i].Key == key)
                {
                    return i;
                }
            }
            return -1;
        }

        private void SelectIndex(int index, bool raise)
        {
            if (index < 0 || index >= _rows.Count)
            {
                return;
            }
            bool changed = _rows[index].Key != _selectedKey;
            _selectedKey = _rows[index].Key;
            Animator.To(this, _highlight, index);
            EnsureVisible(index);
            if (changed && raise)
            {
                Raise(SelectionChanged, this);
            }
        }

        private void EnsureVisible(int index)
        {
            if (index < _scroll)
            {
                _scroll = index;
            }
            else if (index >= _scroll + VisibleRows)
            {
                _scroll = index - VisibleRows + 1;
            }
            ClampScroll();
            Invalidate();
        }

        private void ClampScroll()
        {
            _scroll = Math.Max(0, Math.Min(_scroll, Math.Max(0, _rows.Count - VisibleRows)));
        }

        private int RowAt(int y)
        {
            int i = _scroll + y / Math.Max(1, RowHeight);
            return (i >= 0 && i < _rows.Count) ? i : -1;
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            int h = RowAt(e.Y);
            if (h != _hover)
            {
                _hover = h;
                Cursor = h >= 0 ? Cursors.Hand : Cursors.Default;
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
            int i = RowAt(e.Y);
            if (i >= 0)
            {
                SelectIndex(i, true);
            }
            base.OnMouseClick(e);
        }

        protected override void OnMouseWheel(MouseEventArgs e)
        {
            if (_rows.Count > VisibleRows)
            {
                _scroll -= Math.Sign(e.Delta);
                ClampScroll();
                Invalidate();
                if (e is HandledMouseEventArgs handled)
                {
                    handled.Handled = true;
                }
            }
            base.OnMouseWheel(e);
        }

        protected override bool IsInputKey(Keys keyData) => keyData == Keys.Up || keyData == Keys.Down || base.IsInputKey(keyData);

        protected override void OnKeyDown(KeyEventArgs e)
        {
            int current = IndexOf(_selectedKey);
            if (e.KeyCode == Keys.Up && current > 0)
            {
                SelectIndex(current - 1, true);
                e.Handled = true;
            }
            else if (e.KeyCode == Keys.Down && current < _rows.Count - 1)
            {
                SelectIndex(current + 1, true);
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            int rh = RowHeight;
            if (_rows.Count == 0)
            {
                Draw.Text(g, "No OpenOSV clips yet. Import one above.", TextStyle.Body, new Rectangle(Px(4), 0, Width - Px(8), rh),
                          t.SecondaryText, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
                return;
            }

            // ---- the gliding selection --------------------------------------------------------
            if (IndexOf(_selectedKey) >= 0)
            {
                float y = (float)((_highlight.Value - _scroll) * rh);
                var hl = new RectangleF(0, y + 2f * s, Width, rh - 4f * s);
                Draw.Fill(g, hl, 8f * s, Theme.Mix(Surface, t.Accent, Focused ? 0.24 : 0.16));
            }

            // ---- rows --------------------------------------------------------------------------------
            for (int i = _scroll; i < _rows.Count && i < _scroll + VisibleRows; ++i)
            {
                ClipRow row = _rows[i];
                int top = (i - _scroll) * rh;
                if (i == _hover && row.Key != _selectedKey)
                {
                    Draw.Fill(g, new RectangleF(0, top + 2f * s, Width, rh - 4f * s), 8f * s, Theme.Alpha(t.Text, 0.04));
                }

                // Leading mark: a globe for 360, a lens for a reframed view.
                int icon = Px(28);
                var iconRect = new Rectangle(Px(6), top + (rh - icon) / 2, icon, icon);
                Draw.Fill(g, iconRect, icon / 2f, row.Offline ? Theme.Alpha(t.Red, 0.18) : Theme.Mix(Surface, t.Accent, 0.18));
                Icons.Paint(g, row.Equirect ? Icons.Globe : Icons.Video, iconRect, row.Offline ? t.Red : t.Accent, 10f);

                // Badges on the right.
                int right = Width - Px(8);
                right = Badge(g, row.Offline ? "Offline" : null, t.Red, t.ErrorText, right, top, rh);
                right = Badge(g, row.Proxy ? "LRF" : null, t.Orange, t.WarningText, right, top, rh);
                right = Badge(g, row.EventCount > 1 ? "x" + row.EventCount : null, t.SecondaryText, t.SecondaryText, right, top, rh);

                int textLeft = iconRect.Right + Px(10);
                int textWidth = Math.Max(10, right - textLeft - Px(4));
                Draw.Text(g, row.Name, TextStyle.BodyStrong, new Rectangle(textLeft, top + Px(6), textWidth, Px(18)), t.Text);
                Draw.Text(g, row.Detail, TextStyle.Caption, new Rectangle(textLeft, top + Px(24), textWidth, Px(16)), t.SecondaryText);

                if (i < _rows.Count - 1 && i < _scroll + VisibleRows - 1)
                {
                    using (var pen = new Pen(t.Separator, 1f))
                    {
                        g.DrawLine(pen, textLeft, top + rh - 0.5f, Width - Px(4), top + rh - 0.5f);
                    }
                }
            }

            // ---- a hint that more rows scroll -------------------------------------------------------
            if (_rows.Count > VisibleRows)
            {
                string more = (_scroll + VisibleRows < _rows.Count) ? "Scroll for " + (_rows.Count - _scroll - VisibleRows) + " more" : string.Empty;
                if (more.Length > 0)
                {
                    Draw.Text(g, more, TextStyle.Micro, new Rectangle(0, Height - Px(12), Width - Px(8), Px(12)), t.TertiaryText,
                              TextFormatFlags.Right | TextFormatFlags.Bottom);
                }
            }
        }

        /// <summary>Draw a small capsule badge right-aligned at <paramref name="right"/>; returns the new right edge.</summary>
        private int Badge(Graphics g, string text, Color color, Color ink, int right, int top, int rowHeight)
        {
            if (string.IsNullOrEmpty(text))
            {
                return right;
            }
            Size size = Draw.Measure(g, text, TextStyle.Micro);
            int w = size.Width + Px(10);
            int h = Px(18);
            var r = new Rectangle(right - w, top + (rowHeight - h) / 2, w, h);
            Draw.Fill(g, r, h / 2f, Theme.Alpha(color, 0.18));
            Draw.Text(g, text, TextStyle.Micro, r, ink, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter);
            return r.Left - Px(6);
        }
    }
}
