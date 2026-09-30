// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ImportAction.cs - "Import OSV...": .OSV / .LRF files in, correctly sized,
// correctly long, levels-correct clips with grouped audio out.
//
// ===========================================================================
//  The flow
// ===========================================================================
//   UI thread    find OpenOSV Source and osvtool; plan the batch (pairs,
//                duplicates)
//   worker       per clip: osvtool probe (length, rate, sphere, timestamps)
//                and osvtool extract --audio (a sync-exact float WAV in the
//                cache).  Cancellable; one clip's failure never stops the rest.
//   UI thread    one undo block: per clip, the generated media (file, output,
//                levels, start frame, stabilisation; exact length; frame
//                size), the video and audio events at the cursor, one group.
//
// The VEGAS scripting API is only touched on the UI thread.  The slow part -
// osvtool decoding an hour of AAC - happens before the undo block opens, so
// VEGAS's timeline is never locked while a progress bar crawls.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.Globalization;
using System.IO;
using System.Threading;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using OpenOSV.Vegas.UI;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>The Import OSV command.</summary>
    internal static class ImportAction
    {
        /// <summary>One clip after the worker's pass.</summary>
        private sealed class Prepared
        {
            public ImportItem Item;
            /// <summary>The clip whose timeline the media gets (the .OSV for a proxy import).</summary>
            public ProbeResult Timeline;
            /// <summary>The file the generator plays (the .LRF for a proxy import).</summary>
            public ProbeResult Play;
            public AudioExtraction Audio;
            public string Error;
            public bool Cancelled;
        }

        /// <summary>Ask for files, then import them.</summary>
        public static void RunWithDialog()
        {
            Settings settings = Session.Settings;
            string[] files = null;
            using (var dialog = new OpenFileDialog
            {
                Title = Copy.ImportTitle,
                Filter = Copy.ImportFilter,
                Multiselect = true,
                CheckFileExists = true,
                RestoreDirectory = true,
            })
            {
                if (!string.IsNullOrEmpty(settings.LastImportFolder) && Directory.Exists(settings.LastImportFolder))
                {
                    dialog.InitialDirectory = settings.LastImportFolder;
                }
                if (dialog.ShowDialog(Guard.Owner) != DialogResult.OK)
                {
                    return;
                }
                files = dialog.FileNames;
            }
            Run(files);
        }

        /// <summary>Import files (from the dialog or a drop).</summary>
        public static void Run(IList<string> files)
        {
            // ---- preconditions (UI thread) ------------------------------------------------------
            Project project = VegasHost.Project;
            if (project is null)
            {
                Guard.Report(ActionResult.Blocking(Copy.ImportTitle, Copy.NoProject));
                return;
            }
            PlugInNode generator = VegasHost.FindSourceGenerator();
            if (generator is null)
            {
                Guard.Report(ActionResult.Blocking(Copy.ImportTitle, Copy.NoGenerator, StatusKind.Error));
                return;
            }
            OsvToolLocation where = Session.ToolLocation;
            if (!where.Found)
            {
                Guard.Report(ActionResult.Blocking(Copy.ImportTitle, Copy.NoOsvTool(where.Searched), StatusKind.Error));
                return;
            }
            List<ImportItem> plan = ImportPlan.Plan(files);
            if (plan.Count == 0)
            {
                Guard.Report(ActionResult.Line(Copy.ImportTitle, Copy.NothingToImport, StatusKind.Warning));
                return;
            }
            RememberFolder(plan[0].ChosenPath);
            Settings settings = Session.Settings;
            var tool = new OsvTool(where.ExePath);
            Log.Info("import: " + plan.Count + " file(s) with " + where.ExePath);

            // ---- worker: probe + audio --------------------------------------------------------------------
            List<Prepared> prepared = ProgressDialog.Run(Guard.Owner, "Importing " + Copy.Count(plan.Count, "clip"),
                (progress, cancel) => Prepare(plan, tool, settings, progress, cancel));

            // ---- UI thread: build the timeline in one undo step -----------------------------------------------
            ActionResult result = Place(project, generator, prepared, settings);
            Session.SaveSettings();
            Notifier.RaiseClipsChanged();
            Guard.Report(result);
        }

        private static void RememberFolder(string path)
        {
            try
            {
                string dir = Path.GetDirectoryName(path);
                if (!string.IsNullOrEmpty(dir))
                {
                    Session.Settings.LastImportFolder = dir;
                }
            }
            catch (Exception)
            {
                // Not worth remembering.
            }
        }

        /// <summary>The worker's pass: every probe and audio extraction.  Never touches VEGAS.</summary>
        private static List<Prepared> Prepare(List<ImportItem> plan, OsvTool tool, Settings settings, UI.Progress progress, CancellationToken cancel)
        {
            var list = new List<Prepared>();
            int steps = Math.Max(1, plan.Count * 2);
            int step = 0;
            foreach (ImportItem item in plan)
            {
                var p = new Prepared { Item = item };
                list.Add(p);
                if (item.SkipReason is not null)
                {
                    step += 2;
                    continue;
                }
                if (cancel.IsCancellationRequested)
                {
                    p.Cancelled = true;
                    continue;
                }
                try
                {
                    // ---- probe the timeline clip (and the proxy when it differs) --------------
                    progress.Report((double)step / steps, "Reading " + item.Name + "...");
                    p.Timeline = Session.Probes.Get(tool, item.TimelinePath, cancel, out string error);
                    if (p.Timeline is null)
                    {
                        // A cancel is not a failure of the clip.
                        MarkFailure(p, error, cancel);
                        step += 2;
                        continue;
                    }
                    if (item.Kind == ImportKind.LrfAsProxy)
                    {
                        p.Play = Session.Probes.Get(tool, item.ChosenPath, cancel, out error);
                        if (p.Play is null)
                        {
                            MarkFailure(p, error, cancel);
                            step += 2;
                            continue;
                        }
                        if (!ProxyTiming.Overlaps(p.Timeline, p.Play))
                        {
                            // Recorded at another time: an .LRF on its own timeline.
                            Log.Info("import: '" + item.Name + "' does not overlap its .OSV; imported on its own timeline");
                            p.Timeline = p.Play;
                        }
                    }
                    else
                    {
                        p.Play = p.Timeline;
                    }
                    ++step;

                    // ---- audio from the timeline clip, sync-exact WAV first ---------------------------
                    if (settings.ExtractAudio && p.Timeline.HasAudio && !cancel.IsCancellationRequested)
                    {
                        progress.Report((double)step / steps, "Extracting the audio of " + ProbeResult.FileNameOf(p.Timeline.Path) + "...");
                        AudioExtraction audio = tool.ExtractWav(p.Timeline.Path, Session.Audio, p.Timeline.DurationSeconds, cancel);
                        if (!audio.Succeeded && audio.WavUnsupported && settings.AllowAacFallback)
                        {
                            audio = tool.ExtractAac(p.Timeline.Path, Session.Audio, p.Timeline.DurationSeconds, cancel);
                        }
                        p.Audio = audio;
                        if (audio.Cancelled)
                        {
                            p.Cancelled = true;
                        }
                    }
                    ++step;
                }
                catch (Exception ex)
                {
                    Log.Error("import: preparing '" + item.Name + "' failed", ex);
                    p.Error = ex.Message;
                }
            }
            progress.Report(1.0, "Placing clips on the timeline...");
            return list;
        }

        /// <summary>Record why a clip could not be prepared - or that the user cancelled it.</summary>
        private static void MarkFailure(Prepared p, string error, CancellationToken cancel)
        {
            if (cancel.IsCancellationRequested)
            {
                p.Cancelled = true;
            }
            else
            {
                p.Error = error ?? "osvtool could not read it";
            }
        }

        /// <summary>Build every prepared clip on the timeline, in one undo block.</summary>
        private static ActionResult Place(Project project, PlugInNode generator, List<Prepared> prepared, Settings settings)
        {
            int imported = 0, failed = 0, skipped = 0, cancelled = 0;
            bool aacFallback = false;
            var result = new ActionResult(Copy.ImportTitle, string.Empty, StatusKind.Success);

            VegasHost.FrameSize(project, out int projectW, out int projectH);
            int levels = ProjectColour.OutputLevelsIndexFor(VegasHost.PixelFormatName(project));
            int stabilisation = Stabilisation.IndexFor(settings.RockSteady, settings.HorizonLeveling);
            Timecode position = VegasHost.Cursor;
            VideoTrack videoTrack = null;
            AudioTrack audioTrack = null;

            using (new UndoBlock(project, "Import OSV"))
            {
                foreach (Prepared p in prepared)
                {
                    // ---- the ones that never got here -------------------------------------------------
                    if (p.Item.SkipReason is not null)
                    {
                        ++skipped;
                        result.Add(p.Item.Name + ": skipped, " + p.Item.SkipReason + ".");
                        continue;
                    }
                    // Cancelled while it was being prepared: not placed at all (the
                    // clips finished before the cancel are).
                    if (p.Cancelled)
                    {
                        ++cancelled;
                        continue;
                    }
                    if (p.Error is not null || p.Timeline is null || p.Play is null)
                    {
                        ++failed;
                        result.Add(p.Item.Name + ": " + (p.Error ?? "osvtool could not read it"));
                        continue;
                    }

                    // ---- one clip, fully guarded -----------------------------------------------------------
                    try
                    {
                        if (videoTrack is null)
                        {
                            videoTrack = Timeline.FirstSelectedTrack<VideoTrack>(project) ?? NewVideoTrack(project);
                        }
                        Timecode length = PlaceOne(project, generator, p, settings, projectW, projectH, levels, stabilisation,
                                                   position, videoTrack, ref audioTrack, result, ref aacFallback);
                        if (length is not null)
                        {
                            position = position + length;
                            ++imported;
                        }
                        else
                        {
                            ++failed;
                        }
                    }
                    catch (Exception ex)
                    {
                        ++failed;
                        Log.Error("import: placing '" + p.Item.Name + "' failed", ex);
                        result.Add(p.Item.Name + ": " + ex.Message);
                    }
                }
            }

            // ---- the summary -------------------------------------------------------------------------------
            result.Summary = Copy.ImportDone(imported, failed, skipped, aacFallback);
            if (cancelled > 0)
            {
                result.Summary += " Cancelled before " + Copy.Count(cancelled, "clip") + ".";
            }
            result.Kind = failed > 0 ? (imported > 0 ? StatusKind.Warning : StatusKind.Error)
                                     : (aacFallback || cancelled > 0 ? StatusKind.Warning : StatusKind.Success);
            if (failed > 0)
            {
                result.ForceDialog = true;
            }
            return result;
        }

        /// <summary>
        /// Create one clip's generated media, events, audio and group.  Returns
        /// the video event's length, or null when the clip could not be placed.
        /// </summary>
        private static Timecode PlaceOne(Project project, PlugInNode generator, Prepared p, Settings settings, int projectW, int projectH,
                                         int levels, int stabilisation, Timecode position, VideoTrack videoTrack, ref AudioTrack audioTrack,
                                         ActionResult result, ref bool aacFallback)
        {
            ProbeResult timeline = p.Timeline;
            ProbeResult play = p.Play;
            string name = p.Item.Name;

            // ---- the exact length, from the clip's own rational rate ------------------------------------
            Timecode length = VegasHost.LengthOf(timeline.FrameCount, timeline.Fps);
            if (length is null)
            {
                result.Add(name + ": its length (" + timeline.FrameCount + " frames at " + timeline.Fps + ") makes no sense.");
                return null;
            }

            // ---- the generated media ----------------------------------------------------------------------
            Media media = new Media(generator);
            OsvMedia osv = OsvMedia.TryWrap(media);
            if (osv is null)
            {
                result.Add(name + ": VEGAS made the media, but it is not OpenOSV Source's.");
                return null;
            }

            // Parameters first (the generator opens its clip when `file` changes).
            bool proxyPresented = !ReferenceEquals(timeline, play);
            long startFrame = proxyPresented ? ProxyTiming.MapStartFrame(0, timeline, play) : 0;
            int output = settings.DefaultOutput;
            Ofx.SetString(osv.Fx, SourceParams.File, play.Path);
            Ofx.SetChoice(osv.Fx, SourceParams.Output, output, Choices.Output);
            if (!Ofx.SetChoice(osv.Fx, SourceParams.OutputLevels, levels, Choices.OutputLevels))
            {
                Log.Info("import: this OpenOSV Source has no Output Levels control (an OpenFX bundle from before VEGAS support)");
            }
            Ofx.SetInt(osv.Fx, SourceParams.StartFrame, (int)Math.Min(int.MaxValue, startFrame));
            Ofx.SetChoice(osv.Fx, StitchParams.Stabilization, stabilisation, Choices.Stabilization);

            // Length and frame size.
            media.Length = length;
            ImportPlan.FrameSize(output, projectW, projectH, out int w, out int h);
            VideoStream stream = media.GetVideoStreamByIndex(0);
            if (stream is null)
            {
                result.Add(name + ": the generated media has no video stream.");
                return null;
            }
            stream.Size = new Size(w, h);

            // What the extension remembers (and the comment people see in Project Media).
            var record = new MediaRecord
            {
                Source = timeline.Path,
                Proxy = proxyPresented ? play.Path : (SiblingFiles.ProxyOf(timeline.Path) ?? string.Empty),
                OnProxy = proxyPresented,
                FullStartFrame = 0,
                ProxyStartFrame = proxyPresented ? startFrame : -1,
                Audio = p.Audio is not null && p.Audio.Succeeded ? p.Audio.Path : string.Empty,
                SourceSize = timeline.FileSize,
            };
            osv.SaveRecord(record);

            // ---- the video event --------------------------------------------------------------------------------
            VideoEvent video = videoTrack.AddVideoEvent(position, length);
            AddTake(video, stream, name);
            TrySet(() => video.Loop = false);
            TrySet(() => video.Name = name);

            // ---- the audio event, grouped with it -----------------------------------------------------------------
            if (p.Audio is not null && p.Audio.Succeeded)
            {
                if (!p.Audio.IsWav)
                {
                    aacFallback = true;
                }
                AudioEvent audio = PlaceAudio(project, p.Audio.Path, position, length, name, ref audioTrack, result);
                if (audio is not null)
                {
                    var group = new TrackEventGroup(project);
                    project.TrackEventGroups.Add(group);
                    group.Add(video);
                    group.Add(audio);
                }
            }
            else if (p.Audio is not null && !p.Audio.Cancelled)
            {
                result.Add(name + ": placed without audio (" + (p.Audio.Error ?? "no audio") + ").");
            }
            Log.Info("import: '" + name + "' -> " + timeline.Summary + ", frame " + w + "x" + h + ", output " + Choices.Output[output] +
                     ", levels " + Choices.OutputLevels[levels] + (proxyPresented ? ", playing the proxy from frame " + startFrame : string.Empty));
            return length;
        }

        /// <summary>The clip's audio on the audio track, as long as the video (or shorter, never looped).</summary>
        private static AudioEvent PlaceAudio(Project project, string path, Timecode position, Timecode videoLength, string name,
                                             ref AudioTrack audioTrack, ActionResult result)
        {
            try
            {
                Media audioMedia = project.MediaPool.Find(path) ?? project.MediaPool.AddMedia(path);
                AudioStream stream = audioMedia?.GetAudioStreamByIndex(0);
                if (stream is null)
                {
                    result.Add(name + ": VEGAS could not open the extracted audio (" + Path.GetFileName(path) + ").");
                    return null;
                }
                if (audioTrack is null)
                {
                    audioTrack = Timeline.FirstSelectedTrack<AudioTrack>(project) ?? NewAudioTrack(project);
                }
                Timecode length = VegasHost.Min(videoLength, stream.Length);
                AudioEvent evt = audioTrack.AddAudioEvent(position, length);
                AddTake(evt, stream, name);
                TrySet(() => evt.Loop = false);
                TrySet(() => evt.Name = name);
                return evt;
            }
            catch (Exception ex)
            {
                Log.Error("import: placing the audio of '" + name + "' failed", ex);
                result.Add(name + ": the audio could not be placed (" + ex.Message + ").");
                return null;
            }
        }

        /// <summary>Attach a stream to an event as its active take.</summary>
        internal static Take AddTake(TrackEvent evt, MediaStream stream, string name)
        {
            try
            {
                return evt.AddTake(stream, true, name);
            }
            catch (Exception ex)
            {
                // The documented alternative: construct the take, add it.
                Log.Debug("import: AddTake failed (" + ex.Message + "); adding a new Take instead");
                var take = new Take(stream, true, name);
                evt.Takes.Add(take);
                return take;
            }
        }

        private static VideoTrack NewVideoTrack(Project project)
        {
            VideoTrack track = project.AddVideoTrack();
            TrySet(() => track.Name = "OpenOSV");
            return track;
        }

        private static AudioTrack NewAudioTrack(Project project)
        {
            AudioTrack track = project.AddAudioTrack();
            TrySet(() => track.Name = "OpenOSV audio");
            return track;
        }

        private static void TrySet(Action set)
        {
            try
            {
                set();
            }
            catch (Exception ex)
            {
                Log.Debug("import: an optional setting failed: " + ex.Message);
            }
        }
    }
}
