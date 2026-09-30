// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Stabilisation.cs - DJI Studio's two stabilisation switches and the one
// Stabilisation popup they select.

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// RockSteady and Horizon Leveling, as DJI Studio shows them, mapped onto
    /// the generator's Stabilisation choice (the Premiere panel's table,
    /// docs/PANEL.md "Stabilisation"):
    /// <code>
    /// RockSteady  Horizon Leveling  entry (0-based)
    /// off         off               0  Off
    /// off         on                1  Horizon Lock
    /// on          off               3  Smooth
    /// on          on                4  Smooth + Horizon Lock (the default)
    /// </code>
    /// Entry 2, Full, is the one no pair of switches spells: it locks the view
    /// to the first frame's heading, which DJI Studio has no switch for.
    /// </summary>
    public static class Stabilisation
    {
        /// <summary>The 0-based Stabilisation entry a pair of switches selects.</summary>
        public static int IndexFor(bool rockSteady, bool horizonLeveling)
        {
            if (rockSteady)
            {
                return horizonLeveling ? Choices.StabSmoothLevel : Choices.StabSmooth;
            }
            return horizonLeveling ? Choices.StabHorizonLock : Choices.StabOff;
        }

        /// <summary>
        /// The switches that spell a Stabilisation entry.  Returns false for
        /// Full (and anything out of range): no pair of switches is that entry,
        /// and the outputs then hold DJI's defaults (both on).
        /// </summary>
        public static bool TrySwitchesFor(int index0, out bool rockSteady, out bool horizonLeveling)
        {
            switch (index0)
            {
                case Choices.StabOff: rockSteady = false; horizonLeveling = false; return true;
                case Choices.StabHorizonLock: rockSteady = false; horizonLeveling = true; return true;
                case Choices.StabSmooth: rockSteady = true; horizonLeveling = false; return true;
                case Choices.StabSmoothLevel: rockSteady = true; horizonLeveling = true; return true;
                default: rockSteady = true; horizonLeveling = true; return false;
            }
        }

        /// <summary>A short caption for an entry: "RockSteady + Horizon Leveling".</summary>
        public static string Caption(int index0)
        {
            switch (index0)
            {
                case Choices.StabOff: return "Stabilisation off";
                case Choices.StabHorizonLock: return "Horizon Leveling";
                case Choices.StabFull: return "Full lock (first frame's heading)";
                case Choices.StabSmooth: return "RockSteady";
                case Choices.StabSmoothLevel: return "RockSteady + Horizon Leveling";
                default: return "Unknown stabilisation";
            }
        }
    }
}
