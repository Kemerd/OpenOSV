// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProjectActions.cs - "360 project setup" and "Apply 360 Reframe to selected
// events".

using System;
using System.Collections.Generic;
using System.Drawing;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>Project-level commands.</summary>
    internal static class ProjectActions
    {
        /// <summary>
        /// Make the project a 360 project: a 2:1 frame (its own when it
        /// already is, else 3840 x 1920), VEGAS's 360 output on (so a render
        /// carries the spherical metadata), and the selected OSV clips - or
        /// every OSV clip when nothing is selected - switched to 360 equirect
        /// at exactly the project's size.
        /// </summary>
        public static ActionResult Setup360()
        {
            const string title = "360 project setup";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            VegasHost.FrameSize(project, out int oldW, out int oldH);
            ImportPlan.Project360Size(oldW, oldH, out int w, out int h);

            List<OsvMedia> media = Targets.Media(project, null);
            if (media.Count == 0)
            {
                media = OsvMedia.AllIn(project);
            }
            var result = new ActionResult(title, string.Empty, StatusKind.Success);
            int switched = 0;
            using (new UndoBlock(project, "OpenOSV: 360 project setup"))
            {
                // ---- the frame ---------------------------------------------------------------
                try
                {
                    if (oldW != w || oldH != h)
                    {
                        project.Video.Width = w;
                        project.Video.Height = h;
                    }
                }
                catch (Exception ex)
                {
                    Log.Error("360 setup: setting the frame size failed", ex);
                    result.Add("The frame size could not be set to " + w + " x " + h + ": " + ex.Message);
                    result.Kind = StatusKind.Warning;
                }

                // ---- 360 output ----------------------------------------------------------------
                if (!VegasHost.SetT360Output(project, true))
                {
                    result.Add("This VEGAS has no 360 output setting; set it in Project Properties if your version has one.");
                    result.Kind = StatusKind.Warning;
                }

                // ---- the clips ------------------------------------------------------------------
                foreach (OsvMedia m in media)
                {
                    if (ClipEdit.SetOutput(project, m, Choices.OutputEquirect))
                    {
                        ++switched;
                    }
                }
            }
            result.Summary = Copy.ProjectSetupDone(w, h, switched);
            Notifier.RaiseClipsChanged();
            return result;
        }

        /// <summary>
        /// Put OpenOSV 360 Reframe on every selected event that plays some
        /// other equirect media, after pan/crop, with the whole sphere
        /// stretched over the frame (Timeline.StretchToFill).
        /// </summary>
        public static ActionResult ApplyReframeFilter()
        {
            const string title = "Apply 360 Reframe";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<VideoEvent> events = Timeline.SelectedVideoEvents(project);
            if (events.Count == 0)
            {
                return ActionResult.Line(title, Copy.SelectEventsFirst, StatusKind.Warning);
            }
            PlugInNode filter = VegasHost.FindReframeFilter();
            if (filter is null)
            {
                return ActionResult.Blocking(title, Copy.NoReframeFilter, StatusKind.Error);
            }

            var result = new ActionResult(title, string.Empty, StatusKind.Success);
            int applied = 0, already = 0, osv = 0, animatedPanCrop = 0;
            using (new UndoBlock(project, "OpenOSV: 360 Reframe"))
            {
                foreach (VideoEvent evt in events)
                {
                    try
                    {
                        if (OsvMedia.ForEvent(evt) is not null)
                        {
                            ++osv;
                            continue;
                        }
                        if (Timeline.ReframeEffectOf(evt) is not null)
                        {
                            ++already;
                            continue;
                        }
                        Effect fx = evt.Effects.AddEffect(filter);
                        // Event FX must see the project-size frame the sphere was
                        // stretched into, not the source: after pan/crop.
                        try
                        {
                            fx.ApplyBeforePanCrop = false;
                        }
                        catch (Exception ex)
                        {
                            Log.Warn("reframe: ApplyBeforePanCrop could not be cleared", ex);
                        }
                        if (!Timeline.StretchToFill(evt))
                        {
                            ++animatedPanCrop;
                        }
                        ++applied;
                    }
                    catch (Exception ex)
                    {
                        Log.Error("reframe: adding the filter failed", ex);
                        result.Add(VegasHost.SafeString(() => evt.ActiveTake?.Name) + ": " + ex.Message);
                        result.Kind = StatusKind.Warning;
                    }
                }
            }
            result.Summary = Copy.ReframeApplied(applied, already, osv);
            if (animatedPanCrop > 0)
            {
                result.Summary += " " + Copy.Count(animatedPanCrop, "event") + " kept an animated pan/crop - stretch it to the full frame by hand.";
                result.Kind = StatusKind.Warning;
            }
            return result;
        }
    }

    /// <summary>Per-clip edits the panel's inspector makes.</summary>
    internal static class ClipEdit
    {
        /// <summary>
        /// Output = Reframed view or 360 equirect, with the frame size that
        /// output wants (ImportPlan.FrameSize) and the events that showed the
        /// old full frame moved to the new one.  Caller holds the undo block.
        /// </summary>
        public static bool SetOutput(Project project, OsvMedia m, int output)
        {
            if (m is null)
            {
                return false;
            }
            try
            {
                VegasHost.FrameSize(project, out int pw, out int ph);
                ImportPlan.FrameSize(output, pw, ph, out int w, out int h);
                Size old = m.FrameSize;
                bool ok = Ofx.SetChoice(m.Fx, SourceParams.Output, output, Choices.Output);
                VideoStream stream = m.VideoStream;
                if (stream is not null && (old.Width != w || old.Height != h))
                {
                    stream.Size = new Size(w, h);
                    foreach (VideoEvent evt in m.Events(project))
                    {
                        Timeline.RefitUntouchedPanCrop(evt, old.Width, old.Height, w, h);
                    }
                }
                return ok;
            }
            catch (Exception ex)
            {
                Log.Error("clip: setting the output of '" + m.DisplayName + "' failed", ex);
                return false;
            }
        }

        /// <summary>Run one edit of one clip in its own undo step and report it.</summary>
        public static ActionResult Edit(OsvMedia m, string what, Func<Project, OsvMedia, bool> edit)
        {
            Project project = VegasHost.Project;
            if (project is null || m is null)
            {
                return ActionResult.Line(what, Copy.NoProject, StatusKind.Warning);
            }
            bool ok;
            using (new UndoBlock(project, "OpenOSV: " + what))
            {
                ok = edit(project, m);
            }
            Notifier.RaiseClipsChanged();
            return ActionResult.Line(what, ok ? what + " on " + m.DisplayName + "." : what + " failed on " + m.DisplayName + " - see the log.",
                                     ok ? StatusKind.Success : StatusKind.Error);
        }

        /// <summary>Set every clip's Output Levels to what the project works in.</summary>
        public static ActionResult MatchLevels()
        {
            const string title = "Match levels";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            int index = ProjectColour.OutputLevelsIndexFor(VegasHost.PixelFormatName(project));
            int done = 0, missing = 0;
            using (new UndoBlock(project, "OpenOSV: match levels"))
            {
                foreach (OsvMedia m in OsvMedia.AllIn(project))
                {
                    if (m.OutputLevels < 0)
                    {
                        ++missing;
                    }
                    else if (m.OutputLevels != index && Ofx.SetChoice(m.Fx, SourceParams.OutputLevels, index, Choices.OutputLevels))
                    {
                        ++done;
                    }
                }
            }
            Notifier.RaiseClipsChanged();
            string text = done > 0 ? Choices.OutputLevels[index] + " on " + Copy.Count(done, "clip") + ", matching the project." : "Every clip already matches the project.";
            if (missing > 0)
            {
                text += " " + Copy.Count(missing, "clip") + " without an Output Levels control: update the OpenFX bundle.";
            }
            return ActionResult.Line(title, text, missing > 0 ? StatusKind.Warning : StatusKind.Success);
        }
    }
}
