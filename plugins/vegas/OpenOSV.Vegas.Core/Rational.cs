// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Rational.cs - exact frame rates and the time maths built on them.
//
// A 59.94 fps clip is 60000/1001 frames per second, not 59.94: 215 784 frames
// last 215 999 784 / 60 000 = 3 599.9964 s, not the 3 600 s that
// 215 784 / 59.94 gives - 0.2 frames adrift after an hour, and growing.  Every
// length the extension hands VEGAS is therefore
// computed from the container's own rational (timescale / sample delta), in
// integers, and only rounded once, at the end, to VEGAS's time unit.

using System;
using System.Globalization;

namespace OpenOSV.Vegas.Core
{
    /// <summary>A positive rational number, always stored reduced (e.g. 60000/1001).</summary>
    public readonly struct Rational : IEquatable<Rational>
    {
        /// <summary>Numerator (&gt; 0 for a valid value).</summary>
        public long Num { get; }

        /// <summary>Denominator (&gt; 0 for a valid value).</summary>
        public long Den { get; }

        private Rational(long num, long den)
        {
            Num = num;
            Den = den;
        }

        /// <summary>True when both parts are positive.</summary>
        public bool IsValid => Num > 0 && Den > 0;

        /// <summary>The value as a double (0 when invalid).</summary>
        public double Value => IsValid ? (double)Num / Den : 0.0;

        /// <summary>
        /// Build a reduced rational.  Returns false for a zero or negative part,
        /// or for parts past a million times a million (no real frame rate).
        /// </summary>
        public static bool TryCreate(long num, long den, out Rational value)
        {
            value = default(Rational);
            if (num <= 0 || den <= 0 || num > 1_000_000_000_000L || den > 1_000_000_000_000L)
            {
                return false;
            }
            long g = Gcd(num, den);
            value = new Rational(num / g, den / g);
            return true;
        }

        /// <summary>
        /// The rational a double frame rate most likely is.  The broadcast rates
        /// (23.976, 29.97, 59.94, 119.88 and their integer neighbours) are
        /// recognised within 0.0005; anything else is approximated with a
        /// denominator of at most 1001 by continued fractions.
        /// </summary>
        public static bool TryFromDouble(double fps, out Rational value)
        {
            value = default(Rational);
            if (double.IsNaN(fps) || double.IsInfinity(fps) || fps <= 0.0 || fps > 10000.0)
            {
                return false;
            }
            // ---- the NTSC family first: n * 1000 / 1001 --------------------------
            foreach (long n in new long[] { 24, 30, 48, 60, 120, 240 })
            {
                double ntsc = n * 1000.0 / 1001.0;
                if (Math.Abs(fps - ntsc) < 0.0005)
                {
                    return TryCreate(n * 1000, 1001, out value);
                }
            }
            // ---- whole numbers ------------------------------------------------------
            double rounded = Math.Round(fps);
            if (Math.Abs(fps - rounded) < 0.0005)
            {
                return TryCreate((long)rounded, 1, out value);
            }
            // ---- continued fractions, denominator <= 1001 -----------------------------
            long h0 = 0, h1 = 1, k0 = 1, k1 = 0;
            double x = fps;
            for (int i = 0; i < 32; ++i)
            {
                long a = (long)Math.Floor(x);
                long h2 = a * h1 + h0;
                long k2 = a * k1 + k0;
                if (k2 > 1001)
                {
                    break;
                }
                h0 = h1; h1 = h2; k0 = k1; k1 = k2;
                double frac = x - a;
                if (frac < 1e-12)
                {
                    break;
                }
                x = 1.0 / frac;
            }
            return TryCreate(h1, k1, out value);
        }

        /// <summary>Greatest common divisor of two positive numbers.</summary>
        public static long Gcd(long a, long b)
        {
            a = Math.Abs(a);
            b = Math.Abs(b);
            while (b != 0)
            {
                long t = a % b;
                a = b;
                b = t;
            }
            return a == 0 ? 1 : a;
        }

        /// <inheritdoc/>
        public bool Equals(Rational other) => Num == other.Num && Den == other.Den;

        /// <inheritdoc/>
        public override bool Equals(object obj) => obj is Rational r && Equals(r);

        /// <inheritdoc/>
        public override int GetHashCode() => unchecked((int)(Num * 397) ^ (int)Den);

        /// <summary>"60000/1001".</summary>
        public override string ToString() =>
            Num.ToString(CultureInfo.InvariantCulture) + "/" + Den.ToString(CultureInfo.InvariantCulture);
    }

    /// <summary>Frames, seconds and host time units, converted exactly.</summary>
    public static class TimeMath
    {
        /// <summary>
        /// Seconds that <paramref name="frames"/> frames last at <paramref name="fps"/>
        /// (0 for an invalid rate or a negative count).
        /// </summary>
        public static double FramesToSeconds(long frames, Rational fps)
        {
            if (!fps.IsValid || frames <= 0)
            {
                return 0.0;
            }
            return (double)((decimal)frames * fps.Den / fps.Num);
        }

        /// <summary>
        /// The duration of <paramref name="frames"/> frames in a host time unit
        /// (<paramref name="unitsPerSecond"/> per second), rounded to the
        /// nearest unit.  Exact integer arithmetic in <see cref="decimal"/>
        /// (28 significant digits: a day of 240 fps in 100 ns units is ~10^16).
        /// Returns -1 for invalid input.
        /// </summary>
        public static long FramesToUnits(long frames, Rational fps, long unitsPerSecond)
        {
            if (!fps.IsValid || frames < 0 || unitsPerSecond <= 0)
            {
                return -1;
            }
            try
            {
                decimal exact = (decimal)frames * fps.Den * unitsPerSecond / fps.Num;
                decimal rounded = Math.Round(exact, 0, MidpointRounding.AwayFromZero);
                if (rounded > long.MaxValue)
                {
                    return -1;
                }
                return (long)rounded;
            }
            catch (OverflowException)
            {
                return -1;
            }
        }

        /// <summary>
        /// The frame showing at <paramref name="seconds"/>: floor(seconds * fps)
        /// with the same 1e-4 frame tolerance the generator uses (OfxSource.h,
        /// frameForTime), so an exact boundary never rounds down a frame.
        /// </summary>
        public static long SecondsToFrame(double seconds, Rational fps)
        {
            if (!fps.IsValid || double.IsNaN(seconds) || double.IsInfinity(seconds))
            {
                return 0;
            }
            double frame = seconds * fps.Value + 1e-4;
            if (frame < 0.0)
            {
                return (long)Math.Floor(frame);
            }
            return (long)frame;
        }

        /// <summary>
        /// A duration for people: "0:01.08", "12:05.30", "1:02:03.00".  Never
        /// throws; nonsense is "0:00.00".
        /// </summary>
        public static string FormatDuration(double seconds)
        {
            if (double.IsNaN(seconds) || double.IsInfinity(seconds) || seconds < 0.0)
            {
                seconds = 0.0;
            }
            long hundredths = (long)Math.Round(seconds * 100.0);
            long h = hundredths / 360000;
            long m = (hundredths / 6000) % 60;
            long s = (hundredths / 100) % 60;
            long cs = hundredths % 100;
            var ci = CultureInfo.InvariantCulture;
            if (h > 0)
            {
                return h.ToString(ci) + ":" + m.ToString("00", ci) + ":" + s.ToString("00", ci) + "." + cs.ToString("00", ci);
            }
            return m.ToString(ci) + ":" + s.ToString("00", ci) + "." + cs.ToString("00", ci);
        }

        /// <summary>A frame rate for people: "59.94", "30", "29.97".</summary>
        public static string FormatFps(Rational fps)
        {
            if (!fps.IsValid)
            {
                return "?";
            }
            double v = fps.Value;
            if (fps.Den == 1)
            {
                return fps.Num.ToString(CultureInfo.InvariantCulture);
            }
            return v.ToString("0.###", CultureInfo.InvariantCulture);
        }
    }
}
