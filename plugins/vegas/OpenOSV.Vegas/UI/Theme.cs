// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Theme.cs - the panel's colours, taken from VEGAS's own skin.
//
// ===========================================================================
//  Deference
// ===========================================================================
// The panel sits in a VEGAS dock beside VEGAS's own windows, so it takes
// VEGAS's skin colours (through the scripting API) for its background and
// text instead of inventing a look: in VEGAS's dark skin it is a dark panel,
// in the light skin a light one, and it follows the user's skin change live
// (AppSkinChanged).  Only the semantic colours are the platform's own -
// green "on", blue "act", orange "check this", red "failed" - in their
// dark- or light-appearance variants, chosen by the background's luminance.

using System;
using System.Drawing;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;

namespace OpenOSV.Vegas.UI
{
    /// <summary>One resolved palette.</summary>
    internal sealed class Theme
    {
        /// <summary>The panel background (VEGAS's window background).</summary>
        public Color Background { get; private set; }
        /// <summary>Inset grouped card fill.</summary>
        public Color Card { get; private set; }
        /// <summary>Card hairline border.</summary>
        public Color CardBorder { get; private set; }
        /// <summary>Hairline separators inside a card.</summary>
        public Color Separator { get; private set; }
        /// <summary>Primary text.</summary>
        public Color Text { get; private set; }
        /// <summary>Secondary text (captions, metadata).</summary>
        public Color SecondaryText { get; private set; }
        /// <summary>Tertiary text (placeholders, disabled).</summary>
        public Color TertiaryText { get; private set; }
        /// <summary>The accent (VEGAS's highlight when it has colour, else system blue).</summary>
        public Color Accent { get; private set; }
        /// <summary>Text on the accent.</summary>
        public Color OnAccent { get; private set; }
        /// <summary>A control's resting fill (segmented track, stepper, chip).</summary>
        public Color ControlFill { get; private set; }
        /// <summary>A control's hover fill.</summary>
        public Color ControlHover { get; private set; }
        /// <summary>A control's pressed fill.</summary>
        public Color ControlPressed { get; private set; }
        /// <summary>The raised thumb of a segmented control / switch knob.</summary>
        public Color Thumb { get; private set; }
        /// <summary>Success / on.</summary>
        public Color Green { get; private set; }
        /// <summary>Warning.</summary>
        public Color Orange { get; private set; }
        /// <summary>Failure.</summary>
        public Color Red { get; private set; }
        /// <summary>Warning TEXT: the orange, darkened on a light surface so it still reads (WCAG 3:1+).</summary>
        public Color WarningText { get; private set; }
        /// <summary>Error TEXT: the red, darkened on a light surface.</summary>
        public Color ErrorText { get; private set; }
        /// <summary>True for a dark appearance.</summary>
        public bool Dark { get; private set; }

        /// <summary>The palette in use.</summary>
        public static Theme Current { get; private set; } = Build(Color.FromArgb(45, 45, 45), Color.FromArgb(220, 220, 220), Color.Empty);

        /// <summary>Raised after <see cref="Refresh"/> changed the palette.</summary>
        public static event Action Changed;

        /// <summary>Re-read VEGAS's skin and rebuild the palette.  Never throws.</summary>
        public static void Refresh()
        {
            try
            {
                Color bg = FromSkin("WindowBackground") ?? FromSkin("ButtonFace") ?? SystemColors.Control;
                Color text = FromSkin("WindowText") ?? FromSkin("ButtonText") ?? SystemColors.ControlText;
                Color highlight = FromSkin("Highlight") ?? Color.Empty;
                // A skin that hands back the same colour twice would make
                // invisible text: fall back to the system pair.
                if (Contrast(bg, text) < 3.0)
                {
                    text = Luminance(bg) < 0.5 ? Color.FromArgb(230, 230, 230) : Color.FromArgb(30, 30, 30);
                }
                Current = Build(bg, text, highlight);
                Changed?.Invoke();
            }
            catch (Exception ex)
            {
                Log.Warn("theme: reading VEGAS's skin failed; keeping the last palette", ex);
            }
        }

        /// <summary>Build a palette from a background, a text colour and an optional accent.</summary>
        internal static Theme Build(Color background, Color text, Color highlight)
        {
            bool dark = Luminance(background) < 0.45;
            var t = new Theme
            {
                Dark = dark,
                Background = Opaque(background),
                Text = Opaque(text),
            };
            // ---- surfaces: lifted a step off the background ----------------------------
            t.Card = Mix(t.Background, t.Text, dark ? 0.055 : 0.035);
            t.CardBorder = Mix(t.Background, t.Text, dark ? 0.12 : 0.10);
            t.Separator = Mix(t.Card, t.Text, dark ? 0.12 : 0.10);
            t.SecondaryText = Mix(t.Text, t.Background, 0.38);
            t.TertiaryText = Mix(t.Text, t.Background, 0.58);
            t.ControlFill = Mix(t.Card, t.Text, dark ? 0.09 : 0.07);
            t.ControlHover = Mix(t.Card, t.Text, dark ? 0.14 : 0.11);
            t.ControlPressed = Mix(t.Card, t.Text, dark ? 0.19 : 0.15);
            t.Thumb = dark ? Mix(t.Card, Color.White, 0.22) : Color.White;

            // ---- semantic colours: the platform's, per appearance ---------------------------
            t.Green = dark ? Color.FromArgb(48, 209, 88) : Color.FromArgb(52, 199, 89);
            t.Orange = dark ? Color.FromArgb(255, 159, 10) : Color.FromArgb(255, 149, 0);
            t.Red = dark ? Color.FromArgb(255, 69, 58) : Color.FromArgb(255, 59, 48);
            Color systemBlue = dark ? Color.FromArgb(10, 132, 255) : Color.FromArgb(0, 122, 255);
            // VEGAS's highlight is the accent when it is a real colour, not a grey.
            t.Accent = (!highlight.IsEmpty && Saturation(highlight) > 0.35 && Contrast(highlight, t.Background) > 1.8)
                ? Opaque(highlight)
                : systemBlue;
            t.OnAccent = Luminance(t.Accent) > 0.6 ? Color.FromArgb(20, 20, 20) : Color.White;
            // Orange and red are fill colours; as text on a light card they
            // fall under 3:1, so the text variants are darkened there.
            t.WarningText = dark ? t.Orange : Mix(t.Orange, Color.Black, 0.35);
            t.ErrorText = dark ? t.Red : Mix(t.Red, Color.Black, 0.2);
            return t;
        }

        // ---- colour maths ---------------------------------------------------------------

        /// <summary>Linear mix: 0 = a, 1 = b.</summary>
        public static Color Mix(Color a, Color b, double amount)
        {
            double t = Math.Max(0.0, Math.Min(1.0, amount));
            return Color.FromArgb(
                (int)Math.Round(a.A + (b.A - a.A) * t),
                (int)Math.Round(a.R + (b.R - a.R) * t),
                (int)Math.Round(a.G + (b.G - a.G) * t),
                (int)Math.Round(a.B + (b.B - a.B) * t));
        }

        /// <summary>The colour at an alpha (0..1).</summary>
        public static Color Alpha(Color c, double alpha) =>
            Color.FromArgb((int)Math.Round(Math.Max(0.0, Math.Min(1.0, alpha)) * 255.0), c.R, c.G, c.B);

        /// <summary>Relative luminance (sRGB, 0..1).</summary>
        public static double Luminance(Color c)
        {
            double Lin(int v)
            {
                double s = v / 255.0;
                return s <= 0.04045 ? s / 12.92 : Math.Pow((s + 0.055) / 1.055, 2.4);
            }
            return 0.2126 * Lin(c.R) + 0.7152 * Lin(c.G) + 0.0722 * Lin(c.B);
        }

        /// <summary>WCAG contrast ratio of two colours.</summary>
        public static double Contrast(Color a, Color b)
        {
            double la = Luminance(a) + 0.05;
            double lb = Luminance(b) + 0.05;
            return la > lb ? la / lb : lb / la;
        }

        private static double Saturation(Color c)
        {
            int max = Math.Max(c.R, Math.Max(c.G, c.B));
            int min = Math.Min(c.R, Math.Min(c.G, c.B));
            return max == 0 ? 0.0 : (max - min) / (double)max;
        }

        private static Color Opaque(Color c) => Color.FromArgb(255, c.R, c.G, c.B);

        /// <summary>A VEGAS skin colour (COLORREF 0x00BBGGRR) as a Color, or null.</summary>
        private static Color? FromSkin(string name)
        {
            int? colorRef = VegasHost.SkinColor(name);
            if (!colorRef.HasValue)
            {
                return null;
            }
            try
            {
                return Opaque(ColorTranslator.FromWin32(colorRef.Value));
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
