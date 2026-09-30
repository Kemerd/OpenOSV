// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// MediaActions.cs - "Make framing unique" and "Relink moved OSVs".

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using OpenOSV.Vegas.UI;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>Commands that create or repoint media.</summary>
    internal static class MediaActions
    {
        // =====================================================================
        //  Make framing unique
        // =====================================================================

        /// <summary>
        /// Give each selected OSV event a camera of its own.
        /// </summary>
        /// <remarks>
        /// A generator's parameters live on its media, so every event cut from
        /// one OpenOSV media shares one camera: reframing one cut reframes them
        /// all.  For each selected event whose media also plays elsewhere, this
        /// duplicates the media - every parameter, every keyframe and its
        /// interpolation, the length, the frame size, the record - and moves
        /// that event onto the copy, keeping its take offset, so nothing on
        /// screen changes until the user reframes it.  When every event of a
        /// media is selected, the first keeps the original.
        /// </remarks>
        public static ActionResult MakeFramingUnique()
        {
            const string title = "Make framing unique";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<VideoEvent> selected = Timeline.SelectedVideoEvents(project);
            if (selected.Count == 0)
            {
                return ActionResult.Line(title, Copy.SelectEventsFirst, StatusKind.Warning);
            }
            PlugInNode generator = VegasHost.FindSourceGenerator();
            if (generator is null)
            {
                return ActionResult.Blocking(title, Copy.NoGenerator, StatusKind.Error);
            }

            // ---- group the selection by media ---------------------------------------------
            var byMedia = new Dictionary<string, List<VideoEvent>>(StringComparer.Ordinal);
            var mediaOf = new Dictionary<string, OsvMedia>(StringComparer.Ordinal);
            foreach (VideoEvent evt in selected)
            {
                OsvMedia m = OsvMedia.ForEvent(evt);
                if (m is null)
                {
                    continue;
                }
                if (!byMedia.TryGetValue(m.Key, out List<VideoEvent> list))
                {
                    list = new List<VideoEvent>();
                    byMedia[m.Key] = list;
                    mediaOf[m.Key] = m;
                }
                list.Add(evt);
            }
            if (byMedia.Count == 0)
            {
                return ActionResult.Line(title, Copy.NoOsvInSelection, StatusKind.Warning);
            }

            var result = new ActionResult(title, string.Empty, StatusKind.Success);
            int made = 0, alreadyUnique = 0;
            using (new UndoBlock(project, "OpenOSV: make framing unique"))
            {
                foreach (KeyValuePair<string, List<VideoEvent>> kv in byMedia)
                {
                    OsvMedia source = mediaOf[kv.Key];
                    int total = source.Events(project).Count;
                    List<VideoEvent> events = kv.Value;
                    // All of the media's events selected: the first keeps it.
                    int keep = events.Count >= total ? 1 : 0;
                    if (total <= 1)
                    {
                        ++alreadyUnique;
                        continue;
                    }
                    alreadyUnique += keep;
                    for (int i = keep; i < events.Count; ++i)
                    {
                        try
                        {
                            if (Retarget(events[i], Duplicate(generator, source)))
                            {
                                ++made;
                            }
                            else
                            {
                                result.Add(source.DisplayName + ": one event could not be moved to its copy.");
                            }
                        }
                        catch (Exception ex)
                        {
                            Log.Error("unique: duplicating '" + source.DisplayName + "' failed", ex);
                            result.Add(source.DisplayName + ": " + ex.Message);
                        }
                    }
                }
            }
            result.Summary = Copy.UniqueDone(made, alreadyUnique);
            result.Kind = result.Details.Count > 0 ? StatusKind.Warning : StatusKind.Success;
            Notifier.RaiseClipsChanged();
            return result;
        }

        /// <summary>A new generated media with everything of <paramref name="source"/>.</summary>
        private static OsvMedia Duplicate(PlugInNode generator, OsvMedia source)
        {
            Media media = new Media(generator);
            OsvMedia copy = OsvMedia.TryWrap(media);
            if (copy is null)
            {
                throw new InvalidOperationException("VEGAS made a media that is not OpenOSV Source's");
            }
            int copied = Ofx.CopyAll(source.Fx, copy.Fx);
            Timecode length = source.Length;
            if (!(length is null))
            {
                media.Length = length;
            }
            VideoStream stream = copy.VideoStream;
            if (stream is not null && !source.FrameSize.IsEmpty)
            {
                stream.Size = source.FrameSize;
            }
            copy.SaveRecord(source.RecordOrDerived());
            Log.Info("unique: copied " + copied + " parameters of '" + source.DisplayName + "' into a new media");
            return copy;
        }

        /// <summary>Move an event onto another media, same offset, same name; drop the old take.</summary>
        private static bool Retarget(VideoEvent evt, OsvMedia to)
        {
            Take old = evt.ActiveTake;
            VideoStream stream = to.VideoStream;
            if (old is null || stream is null)
            {
                return false;
            }
            Timecode offset = old.Offset;
            string name = VegasHost.SafeString(() => old.Name) ?? to.DisplayName;
            Take fresh = ImportAction.AddTake(evt, stream, name);
            if (fresh is null)
            {
                return false;
            }
            try
            {
                fresh.Offset = offset;
            }
            catch (Exception ex)
            {
                Log.Warn("unique: the new take's offset could not be set", ex);
            }
            try
            {
                evt.Takes.Remove(old);
            }
            catch (Exception ex)
            {
                // The old take stays as an inactive alternative: harmless.
                Log.Debug("unique: removing the old take failed: " + ex.Message);
            }
            return true;
        }

        // =====================================================================
        //  Relink moved OSVs
        // =====================================================================

        /// <summary>One clip that is missing on disk.</summary>
        private sealed class Missing
        {
            public OsvMedia Media;
            public MediaRecord Record;
            public string OldPath;
            public string Found;
            public string OldAudio;
            public AudioExtraction NewAudio;
        }

        /// <summary>
        /// Find every OpenOSV clip whose file is gone, search a folder the user
        /// picks for the same file names, and point the clips there.  An
        /// extracted audio file that went missing too is extracted again from
        /// the relinked clip and swapped in on the timeline.
        /// </summary>
        public static ActionResult Relink()
        {
            const string title = "Relink moved OSVs";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            var missing = new List<Missing>();
            foreach (OsvMedia m in OsvMedia.AllIn(project))
            {
                if (!m.IsOffline)
                {
                    continue;
                }
                MediaRecord r = m.RecordOrDerived();
                string old = m.FilePath;
                if (string.IsNullOrEmpty(old))
                {
                    old = r.Source;
                }
                if (!string.IsNullOrEmpty(old))
                {
                    missing.Add(new Missing { Media = m, Record = r, OldPath = old, OldAudio = r.Audio });
                }
            }
            if (missing.Count == 0)
            {
                return ActionResult.Line(title, Copy.NothingMissing, StatusKind.Success);
            }

            // ---- where to look -----------------------------------------------------------------
            string root;
            using (var dialog = new FolderBrowserDialog
            {
                Description = "Where did the " + Copy.Count(missing.Count, "clip") + " go? Pick a folder; OpenOSV searches everything under it.",
                ShowNewFolderButton = false,
            })
            {
                string last = Session.Settings.LastRelinkFolder;
                if (!string.IsNullOrEmpty(last) && Directory.Exists(last))
                {
                    dialog.SelectedPath = last;
                }
                if (dialog.ShowDialog(Guard.Owner) != DialogResult.OK || string.IsNullOrEmpty(dialog.SelectedPath))
                {
                    return ActionResult.Line(title, "Relink cancelled.", StatusKind.Info);
                }
                root = dialog.SelectedPath;
            }
            Session.Settings.LastRelinkFolder = root;
            Session.SaveSettings();
            OsvTool tool = Session.Tool;
            bool extractAudio = Session.Settings.ExtractAudio;

            // ---- worker: one walk for every name, then any audio to redo --------------------------
            ProgressDialog.Run(Guard.Owner, "Relinking " + Copy.Count(missing.Count, "clip"), (progress, cancel) =>
            {
                var names = missing.Select(x => ProbeResult.FileNameOf(x.OldPath)).ToList();
                progress.Report(0.05, "Searching " + root + "...");
                Dictionary<string, List<string>> found = RelinkSearch.FindByName(root, names, cancel,
                    folders => progress.Report(0.05, "Searched " + folders + " folders..."));
                int i = 0;
                foreach (Missing x in missing)
                {
                    ++i;
                    string name = ProbeResult.FileNameOf(x.OldPath);
                    if (found.TryGetValue(name, out List<string> candidates))
                    {
                        long size = SiblingFiles.IsLrf(x.OldPath) ? 0 : x.Record.SourceSize;
                        x.Found = RelinkSearch.PickBest(x.OldPath, candidates, size);
                    }
                    // The audio went missing too: extract it again from the clip's
                    // full-quality original (the audio belongs to its timeline).
                    bool audioGone = !string.IsNullOrEmpty(x.OldAudio) && !SiblingFiles.Exists(x.OldAudio);
                    if (x.Found is not null && audioGone && extractAudio && tool is not null && !cancel.IsCancellationRequested)
                    {
                        string original = SiblingFiles.IsLrf(x.Found) ? (SiblingFiles.OriginalOf(x.Found) ?? x.Found) : x.Found;
                        progress.Report(0.5 + 0.5 * i / missing.Count, "Extracting the audio of " + ProbeResult.FileNameOf(original) + "...");
                        ProbeResult probe = Session.Probes.Get(tool, original, cancel, out _);
                        double seconds = probe?.DurationSeconds ?? 0;
                        x.NewAudio = tool.ExtractWav(original, Session.Audio, seconds, cancel);
                        if (!x.NewAudio.Succeeded && x.NewAudio.WavUnsupported)
                        {
                            x.NewAudio = tool.ExtractAac(original, Session.Audio, seconds, cancel);
                        }
                    }
                }
                progress.Report(1.0, "Relinking...");
                return true;
            });

            // ---- UI thread: repoint -------------------------------------------------------------------
            var result = new ActionResult(title, string.Empty, StatusKind.Success);
            int relinked = 0, stillMissing = 0, audio = 0;
            using (new UndoBlock(project, "OpenOSV: relink"))
            {
                foreach (Missing x in missing)
                {
                    if (x.Found is null)
                    {
                        ++stillMissing;
                        result.Add(ProbeResult.FileNameOf(x.OldPath) + ": not found under " + root + ".");
                        continue;
                    }
                    try
                    {
                        if (!Ofx.SetString(x.Media.Fx, SourceParams.File, x.Found))
                        {
                            ++stillMissing;
                            continue;
                        }
                        ++relinked;
                        // The record follows the move: the new source, and the
                        // pair beside it.
                        if (SiblingFiles.IsLrf(x.Found))
                        {
                            x.Record.Proxy = x.Found;
                            x.Record.Source = SiblingFiles.OriginalOf(x.Found) ?? x.Record.Source;
                        }
                        else
                        {
                            x.Record.Source = x.Found;
                            x.Record.Proxy = SiblingFiles.ProxyOf(x.Found) ?? x.Record.Proxy;
                        }
                        if (x.NewAudio is not null && x.NewAudio.Succeeded && ReplaceAudio(project, x.OldAudio, x.NewAudio.Path))
                        {
                            x.Record.Audio = x.NewAudio.Path;
                            ++audio;
                        }
                        x.Media.SaveRecord(x.Record);
                        Log.Info("relink: '" + x.OldPath + "' -> '" + x.Found + "'");
                    }
                    catch (Exception ex)
                    {
                        ++stillMissing;
                        Log.Error("relink: '" + x.OldPath + "' failed", ex);
                        result.Add(ProbeResult.FileNameOf(x.OldPath) + ": " + ex.Message);
                    }
                }
            }
            result.Summary = Copy.RelinkDone(relinked, stillMissing, audio);
            result.Kind = stillMissing > 0 ? StatusKind.Warning : StatusKind.Success;
            Notifier.RaiseClipsChanged();
            return result;
        }

        /// <summary>Swap every use of a missing audio file for a freshly extracted one.</summary>
        private static bool ReplaceAudio(Project project, string oldPath, string newPath)
        {
            try
            {
                Media old = string.IsNullOrEmpty(oldPath) ? null : project.MediaPool.Find(oldPath);
                if (old is null)
                {
                    return false;
                }
                Media fresh = project.MediaPool.Find(newPath) ?? project.MediaPool.AddMedia(newPath);
                if (fresh is null)
                {
                    return false;
                }
                old.ReplaceWith(fresh);
                return true;
            }
            catch (Exception ex)
            {
                Log.Warn("relink: replacing the audio '" + oldPath + "' failed", ex);
                return false;
            }
        }
    }
}
