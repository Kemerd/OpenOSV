// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Tiles.cs - the easing preset tiles and the drop zone.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.IO;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// One of DJI Studio's keyframe easing presets as a picture: the speed
    /// profile between two keyframe dots (the curves the effect renders -
    /// DjiCamera.EaseSpeed), or a crossed circle for None.
    /// </summary>
    internal sealed class EasingTile : OsvControl
    {
        private readonly Spring _press = new Spring(0, 0.18);
        private bool _selected;

        public EasingTile(EasingPreset preset)
        {
            Preset = preset;
            MakeFocusable();
            Cursor = Cursors.Hand;
        }

        /// <summary>The preset drawn.</summary>
        public EasingPreset Preset { get; }

        /// <summary>Shown as the chosen preset.</summary>
        public bool Selected
        {
            get => _selected;
            set
            {
                _selected = value;
                Invalidate();
            }
        }

        /// <summary>Tile size on the 8-point grid.</summary>
        public Size PreferredTileSize => new Size(Px(72), Px(76));

        protected override void OnVisualStateChanged() => Animator.To(this, _press, Pressed ? 1 : 0);

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (Enabled && (e.KeyCode == Keys.Enter || e.KeyCode == Keys.Space))
            {
                OnClick(EventArgs.Empty);
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            var r = new RectangleF(1f * s, 1f * s, Width - 2f * s, Height - 2f * s);
            Color fill = _selected ? Theme.Mix(Surface, t.Accent, 0.22) : (Hovered ? t.ControlHover : t.ControlFill);
            fill = Theme.Mix(fill, Surface, 0.25 * _press.Value);
            Draw.Fill(g, r, 9f * s, fill);
            if (_selected)
            {
                Draw.Stroke(g, RectangleF.Inflate(r, -0.75f * s, -0.75f * s), 8.5f * s, t.Accent, 1.5f * s);
            }

            // ---- the curve, above a clear band for the two-line caption ---------------
            int lineH = Px(12);
            float captionBand = 2 * lineH + 6f * s;
            var art = new RectangleF(r.X + 12f * s, r.Y + 9f * s, r.Width - 24f * s, Math.Max(8f * s, r.Height - captionBand - 17f * s));
            Color ink = _selected ? t.Accent : t.SecondaryText;
            if (Preset.Id == "none")
            {
                float d = Math.Min(art.Width, art.Height) * 0.8f;
                var circle = new RectangleF(art.X + (art.Width - d) / 2f, art.Y + (art.Height - d) / 2f, d, d);
                using (var pen = new Pen(ink, 1.5f * s))
                {
                    g.DrawEllipse(pen, circle);
                    g.DrawLine(pen, circle.X + d * 0.2f, circle.Bottom - d * 0.2f, circle.Right - d * 0.2f, circle.Y + d * 0.2f);
                }
            }
            else
            {
                const int steps = 24;
                var points = new PointF[steps + 1];
                for (int i = 0; i <= steps; ++i)
                {
                    double u = i / (double)steps;
                    double speed = DjiCamera.EaseSpeed(Preset.Id, u);
                    points[i] = new PointF(art.X + (float)(art.Width * u), art.Bottom - (float)(art.Height * speed / 2.0));
                }
                using (var pen = new Pen(ink, 1.75f * s) { LineJoin = LineJoin.Round, StartCap = LineCap.Round, EndCap = LineCap.Round })
                {
                    g.DrawLines(pen, points);
                }
                float dot = 4.5f * s;
                using (var b = new SolidBrush(ink))
                {
                    g.FillEllipse(b, points[0].X - dot / 2f, points[0].Y - dot / 2f, dot, dot);
                    g.FillEllipse(b, points[steps].X - dot / 2f, points[steps].Y - dot / 2f, dot, dot);
                }
            }

            // ---- two-line caption --------------------------------------------------------------
            Color text = _selected ? t.Text : t.SecondaryText;
            int captionTop = (int)(r.Bottom - captionBand);
            Draw.Text(g, Preset.Line1, TextStyle.Micro, new Rectangle((int)r.X, captionTop, (int)r.Width, lineH), text,
                      TextFormatFlags.HorizontalCenter | TextFormatFlags.Top | TextFormatFlags.EndEllipsis);
            Draw.Text(g, Preset.Line2, TextStyle.Micro, new Rectangle((int)r.X, captionTop + lineH, (int)r.Width, lineH), text,
                      TextFormatFlags.HorizontalCenter | TextFormatFlags.Top | TextFormatFlags.EndEllipsis);
            if (ShowFocus)
            {
                Draw.FocusRing(g, r, 9f * s, s);
            }
        }
    }

    /// <summary>
    /// Drop .OSV / .LRF files from Explorer here; click it to browse.  It
    /// lights up (a spring on the glow) while a drag with clips hovers.
    /// </summary>
    internal sealed class DropZone : OsvControl
    {
        private readonly Spring _glow = new Spring(0, 0.25);
        private bool _dragOk;

        public DropZone()
        {
            AllowDrop = true;
            Cursor = Cursors.Hand;
        }

        /// <summary>Raised with the clip files dropped (already filtered to .OSV / .LRF).</summary>
        public event Action<IList<string>> FilesDropped;

        /// <summary>The height this control wants.</summary>
        public int PreferredHeight => Px(72);

        private static List<string> ClipsIn(DragEventArgs e)
        {
            try
            {
                if (e.Data is not null && e.Data.GetDataPresent(DataFormats.FileDrop) && e.Data.GetData(DataFormats.FileDrop) is string[] files)
                {
                    return SiblingFiles.FilterClips(files);
                }
            }
            catch (Exception ex)
            {
                Log.Debug("drop: reading the dragged files failed: " + ex.Message);
            }
            return new List<string>();
        }

        protected override void OnDragEnter(DragEventArgs e)
        {
            _dragOk = ClipsIn(e).Count > 0;
            e.Effect = _dragOk ? DragDropEffects.Copy : DragDropEffects.None;
            Animator.To(this, _glow, _dragOk ? 1 : 0);
            base.OnDragEnter(e);
        }

        protected override void OnDragOver(DragEventArgs e)
        {
            e.Effect = _dragOk ? DragDropEffects.Copy : DragDropEffects.None;
            base.OnDragOver(e);
        }

        protected override void OnDragLeave(EventArgs e)
        {
            _dragOk = false;
            Animator.To(this, _glow, 0);
            base.OnDragLeave(e);
        }

        protected override void OnDragDrop(DragEventArgs e)
        {
            List<string> clips = ClipsIn(e);
            _dragOk = false;
            Animator.To(this, _glow, 0);
            base.OnDragDrop(e);
            if (clips.Count == 0)
            {
                return;
            }
            try
            {
                // Let Explorer's drag finish before an import dialog takes over.
                BeginInvoke(new Action(() => FilesDropped?.Invoke(clips)));
            }
            catch (Exception ex)
            {
                Log.Warn("drop: handing the files over failed", ex);
            }
        }

        protected override void OnVisualStateChanged()
        {
            if (!_dragOk)
            {
                Animator.To(this, _glow, Hovered ? 0.35 : 0);
            }
        }

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            float s = S;
            var r = new RectangleF(1.5f * s, 1.5f * s, Width - 3f * s, Height - 3f * s);
            double glow = _glow.Value;
            Draw.Fill(g, r, 10f * s, Theme.Mix(Surface, t.Accent, 0.04 + 0.14 * glow));
            using (GraphicsPath p = Draw.Rounded(r, 10f * s))
            using (var pen = new Pen(Theme.Mix(t.Separator, t.Accent, glow), 1.25f * s) { DashStyle = DashStyle.Dash, DashPattern = new[] { 4f, 3f } })
            {
                g.DrawPath(pen, p);
            }
            Color ink = Theme.Mix(t.SecondaryText, t.Accent, glow);
            bool icon = Icons.Font(14f) is not null;
            int iconH = icon ? Px(22) : 0;
            int top = (Height - iconH - Px(34)) / 2;
            if (icon)
            {
                Icons.Paint(g, Icons.Download, new Rectangle(0, top, Width, iconH), ink, 14f);
            }
            Draw.Text(g, glow > 0.6 ? "Let go to import" : "Drop .OSV or .LRF files here", TextStyle.BodyStrong,
                      new Rectangle(Px(8), top + iconH, Width - Px(16), Px(18)), glow > 0.6 ? t.Accent : t.Text,
                      TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
            Draw.Text(g, "or click to browse", TextStyle.Caption, new Rectangle(Px(8), top + iconH + Px(18), Width - Px(16), Px(16)),
                      t.SecondaryText, TextFormatFlags.HorizontalCenter | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis);
        }
    }
}
