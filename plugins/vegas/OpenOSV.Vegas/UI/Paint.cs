// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Paint.cs - typography and the drawing primitives every control shares.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;
using System.Windows.Forms;

namespace OpenOSV.Vegas.UI
{
    /// <summary>The panel's type ramp.</summary>
    internal enum TextStyle
    {
        /// <summary>Panel title (Display, semibold).</summary>
        Title,
        /// <summary>A card's section header (small, semibold, secondary colour).</summary>
        Section,
        /// <summary>Body text and control labels.</summary>
        Body,
        /// <summary>Emphasised body (a clip's name).</summary>
        BodyStrong,
        /// <summary>Captions and metadata.</summary>
        Caption,
        /// <summary>Tiny labels (badges, tile captions).</summary>
        Micro,
    }

    /// <summary>
    /// Fonts: Segoe UI Variable on Windows 11 (Display for the title, Text for
    /// everything else, Small for tiny labels), Segoe UI where it is missing.
    /// Sizes are in points, so they follow the monitor's DPI like the rest of
    /// VEGAS.
    /// </summary>
    internal static class Typeface
    {
        private static readonly Dictionary<TextStyle, Font> Cache = new Dictionary<TextStyle, Font>();
        private static string _text;
        private static string _display;
        private static string _small;

        /// <summary>The font for a style (cached; never disposed while VEGAS runs).</summary>
        public static Font Of(TextStyle style)
        {
            if (Cache.TryGetValue(style, out Font f))
            {
                return f;
            }
            ResolveFamilies();
            switch (style)
            {
                case TextStyle.Title: f = Make(_display, 13.5f, FontStyle.Bold); break;
                case TextStyle.Section: f = Make(_text, 8.25f, FontStyle.Bold); break;
                case TextStyle.BodyStrong: f = Make(_text, 9.25f, FontStyle.Bold); break;
                case TextStyle.Caption: f = Make(_text, 8.25f, FontStyle.Regular); break;
                case TextStyle.Micro: f = Make(_small, 7.5f, FontStyle.Regular); break;
                default: f = Make(_text, 9.25f, FontStyle.Regular); break;
            }
            Cache[style] = f;
            return f;
        }

        private static void ResolveFamilies()
        {
            if (_text is not null)
            {
                return;
            }
            var installed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
            try
            {
                using (var fonts = new InstalledFontCollection())
                {
                    foreach (FontFamily family in fonts.Families)
                    {
                        installed.Add(family.Name);
                    }
                }
            }
            catch (Exception)
            {
                // No font list: Segoe UI is on every supported Windows.
            }
            _text = installed.Contains("Segoe UI Variable Text") ? "Segoe UI Variable Text" : "Segoe UI";
            _display = installed.Contains("Segoe UI Variable Display") ? "Segoe UI Variable Display" : _text;
            _small = installed.Contains("Segoe UI Variable Small") ? "Segoe UI Variable Small" : _text;
        }

        private static Font Make(string family, float points, FontStyle style)
        {
            try
            {
                // Semibold where the family has it (Variable fonts expose it as a
                // named face), bold otherwise.
                if (style == FontStyle.Bold)
                {
                    try
                    {
                        var semibold = new Font(family + " Semibold", points, FontStyle.Regular, GraphicsUnit.Point);
                        if (semibold.Name.IndexOf("Semibold", StringComparison.OrdinalIgnoreCase) >= 0)
                        {
                            return semibold;
                        }
                        semibold.Dispose();
                    }
                    catch (Exception)
                    {
                        // No semibold face: bold it is.
                    }
                }
                return new Font(family, points, style, GraphicsUnit.Point);
            }
            catch (Exception)
            {
                return new Font(FontFamily.GenericSansSerif, points, style, GraphicsUnit.Point);
            }
        }
    }

    /// <summary>Drawing helpers.</summary>
    internal static class Draw
    {
        /// <summary>Anti-aliased shapes, crisp text.</summary>
        public static void Prepare(Graphics g)
        {
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.PixelOffsetMode = PixelOffsetMode.HighQuality;
            g.InterpolationMode = InterpolationMode.HighQualityBicubic;
            g.TextRenderingHint = TextRenderingHint.ClearTypeGridFit;
        }

        /// <summary>A rounded rectangle path (radius clamped to the rectangle).</summary>
        public static GraphicsPath Rounded(RectangleF r, float radius)
        {
            var path = new GraphicsPath();
            float rad = Math.Max(0f, Math.Min(radius, Math.Min(r.Width, r.Height) / 2f));
            if (rad < 0.5f)
            {
                path.AddRectangle(r);
                return path;
            }
            float d = rad * 2f;
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            return path;
        }

        /// <summary>Fill a rounded rectangle.</summary>
        public static void Fill(Graphics g, RectangleF r, float radius, Color color)
        {
            if (r.Width <= 0 || r.Height <= 0 || color.A == 0)
            {
                return;
            }
            using (GraphicsPath p = Rounded(r, radius))
            using (var b = new SolidBrush(color))
            {
                g.FillPath(b, p);
            }
        }

        /// <summary>Stroke a rounded rectangle.</summary>
        public static void Stroke(Graphics g, RectangleF r, float radius, Color color, float width)
        {
            if (r.Width <= 0 || r.Height <= 0 || color.A == 0)
            {
                return;
            }
            using (GraphicsPath p = Rounded(r, radius))
            using (var pen = new Pen(color, width))
            {
                g.DrawPath(pen, p);
            }
        }

        /// <summary>The keyboard focus ring: 2 px of accent, 2 px outside the shape.</summary>
        public static void FocusRing(Graphics g, RectangleF r, float radius, float scale)
        {
            float inset = -2f * scale;
            RectangleF ring = RectangleF.Inflate(r, -inset, -inset);
            Stroke(g, ring, radius + 2f * scale, Theme.Alpha(Theme.Current.Accent, 0.85), 2f * scale);
        }

        /// <summary>
        /// Text with GDI (ClearType, the same renderer VEGAS's own UI uses).
        /// GDI has no alpha, so fading text is drawn in a colour mixed toward
        /// the background instead.
        /// </summary>
        public static void Text(Graphics g, string text, TextStyle style, Rectangle bounds, Color color,
                                TextFormatFlags flags = TextFormatFlags.Left | TextFormatFlags.VerticalCenter | TextFormatFlags.EndEllipsis)
        {
            if (string.IsNullOrEmpty(text) || bounds.Width <= 0 || bounds.Height <= 0)
            {
                return;
            }
            TextRenderer.DrawText(g, text, Typeface.Of(style), bounds, color, flags | TextFormatFlags.NoPrefix);
        }

        /// <summary>Measure one line (or wrapped text within <paramref name="width"/>).</summary>
        public static Size Measure(Graphics g, string text, TextStyle style, int width = int.MaxValue, bool wrap = false)
        {
            if (string.IsNullOrEmpty(text))
            {
                return Size.Empty;
            }
            TextFormatFlags flags = TextFormatFlags.NoPrefix | (wrap ? TextFormatFlags.WordBreak : TextFormatFlags.SingleLine);
            return TextRenderer.MeasureText(g, text, Typeface.Of(style), new Size(Math.Max(1, width), int.MaxValue), flags);
        }

        /// <summary>Measure without a Graphics (layout passes).</summary>
        public static Size Measure(string text, TextStyle style, int width = int.MaxValue, bool wrap = false)
        {
            if (string.IsNullOrEmpty(text))
            {
                return Size.Empty;
            }
            TextFormatFlags flags = TextFormatFlags.NoPrefix | (wrap ? TextFormatFlags.WordBreak : TextFormatFlags.SingleLine);
            return TextRenderer.MeasureText(text, Typeface.Of(style), new Size(Math.Max(1, width), int.MaxValue), flags);
        }

        /// <summary>A chevron (for dropdowns and disclosure), rotated by <paramref name="angle"/> degrees.</summary>
        public static void Chevron(Graphics g, PointF centre, float size, float angle, Color color, float width)
        {
            var state = g.Save();
            try
            {
                g.TranslateTransform(centre.X, centre.Y);
                g.RotateTransform(angle);
                using (var pen = new Pen(color, width) { StartCap = LineCap.Round, EndCap = LineCap.Round, LineJoin = LineJoin.Round })
                {
                    g.DrawLines(pen, new[] { new PointF(-size, -size / 2f), new PointF(0, size / 2f), new PointF(size, -size / 2f) });
                }
            }
            finally
            {
                g.Restore(state);
            }
        }
    }
}
