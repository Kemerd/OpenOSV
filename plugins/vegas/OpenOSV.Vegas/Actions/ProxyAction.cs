// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ProxyAction.cs - "Edit with LRF proxies" / "Full quality": swap every
// OpenOSV clip between its .OSV and the .LRF the camera wrote beside it,
// keeping every frame on the same moment (ProxyTiming.cs).

using System;
using System.Collections.Generic;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using OpenOSV.Vegas.UI;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Actions
{
    /// <summary>The proxy switch.</summary>
    internal static class ProxyAction
    {
        /// <summary>One media's planned swap.</summary>
        private sealed class Swap
        {
            public OsvMedia Media;
            public MediaRecord Record;
            public string From;
            public string To;
            public ProbeResult FromProbe;
            public ProbeResult ToProbe;
            public string Error;
        }

        /// <summary>The project's proxy state: true all on proxies, false all full, null mixed or none.</summary>
        public static bool? State(Project project)
        {
            List<OsvMedia> all = OsvMedia.AllIn(project);
            if (all.Count == 0)
            {
                return null;
            }
            int proxies = 0;
            foreach (OsvMedia m in all)
            {
                if (m.OnProxy)
                {
                    ++proxies;
                }
            }
            if (proxies == 0)
            {
                return false;
            }
            return proxies == all.Count ? true : (bool?)null;
        }

        /// <summary>Switch every OpenOSV clip to its proxy (true) or back to full quality (false).</summary>
        public static ActionResult Run(bool toProxy)
        {
            string title = toProxy ? "Edit with LRF proxies" : "Full quality";
            Project project = VegasHost.Project;
            if (project is null)
            {
                return ActionResult.Blocking(title, Copy.NoProject);
            }
            List<OsvMedia> all = OsvMedia.AllIn(project);
            if (all.Count == 0)
            {
                return ActionResult.Line(title, Copy.NoOsvMedia, StatusKind.Info);
            }

            // ---- plan (UI thread: reads parameters and the file system) -----------------------
            var swaps = new List<Swap>();
            int already = 0, noSibling = 0;
            foreach (OsvMedia m in all)
            {
                string file = m.FilePath;
                MediaRecord record = m.RecordOrDerived();
                if (toProxy)
                {
                    if (SiblingFiles.IsLrf(file))
                    {
                        ++already;
                        continue;
                    }
                    string lrf = SiblingFiles.ProxyOf(file);
                    if (lrf is null && SiblingFiles.IsLrf(record.Proxy) && SiblingFiles.Exists(record.Proxy))
                    {
                        lrf = record.Proxy;
                    }
                    if (lrf is null)
                    {
                        ++noSibling;
                        continue;
                    }
                    swaps.Add(new Swap { Media = m, Record = record, From = file, To = lrf });
                }
                else
                {
                    if (!SiblingFiles.IsLrf(file))
                    {
                        ++already;
                        continue;
                    }
                    string osv = SiblingFiles.IsOsv(record.Source) && SiblingFiles.Exists(record.Source) ? record.Source : SiblingFiles.OriginalOf(file);
                    if (osv is null)
                    {
                        ++noSibling;
                        continue;
                    }
                    swaps.Add(new Swap { Media = m, Record = record, From = file, To = osv });
                }
            }
            if (swaps.Count == 0)
            {
                return ActionResult.Line(title, Copy.ProxyDone(toProxy, 0, already, noSibling, 0), noSibling > 0 ? StatusKind.Warning : StatusKind.Info);
            }
            // osvtool is looked up here, on the UI thread; the worker only runs it.
            OsvTool tool = Session.Tool;
            if (tool is null)
            {
                return ActionResult.Blocking(title, Copy.NoOsvTool(Session.ToolLocation.Searched), StatusKind.Error);
            }

            // ---- worker: probe both sides (cached) --------------------------------------------------
            ProgressDialog.Run(Guard.Owner, toProxy ? "Switching to LRF proxies" : "Switching to full quality", (progress, cancel) =>
            {
                for (int i = 0; i < swaps.Count && !cancel.IsCancellationRequested; ++i)
                {
                    Swap s = swaps[i];
                    progress.Report((double)i / swaps.Count, "Reading " + ProbeResult.FileNameOf(s.To) + "...");
                    s.FromProbe = Session.Probes.Get(tool, s.From, cancel, out string e1);
                    s.ToProbe = s.FromProbe is null ? null : Session.Probes.Get(tool, s.To, cancel, out e1);
                    if (s.FromProbe is null || s.ToProbe is null)
                    {
                        s.Error = e1 ?? "cancelled";
                    }
                }
                progress.Report(1.0, "Switching...");
                return true;
            });

            // ---- UI thread: one undo step ---------------------------------------------------------
            var result = new ActionResult(title, string.Empty, StatusKind.Success);
            int switched = 0, failed = 0;
            using (new UndoBlock(project, "OpenOSV: " + title))
            {
                foreach (Swap s in swaps)
                {
                    if (s.Error is not null || s.FromProbe is null || s.ToProbe is null)
                    {
                        ++failed;
                        result.Add(s.Media.DisplayName + ": " + (s.Error ?? "cancelled"));
                        continue;
                    }
                    try
                    {
                        if (Apply(s, toProxy))
                        {
                            ++switched;
                        }
                        else
                        {
                            ++failed;
                            result.Add(s.Media.DisplayName + ": the generator refused the new file.");
                        }
                    }
                    catch (Exception ex)
                    {
                        ++failed;
                        Log.Error("proxy: switching '" + s.Media.DisplayName + "' failed", ex);
                        result.Add(s.Media.DisplayName + ": " + ex.Message);
                    }
                }
            }
            result.Summary = Copy.ProxyDone(toProxy, switched, already, noSibling, failed);
            result.Kind = failed > 0 ? StatusKind.Warning : StatusKind.Success;
            Notifier.RaiseClipsChanged();
            return result;
        }

        /// <summary>
        /// Point one media at the other file and move Start Frame (and any
        /// Start Frame keyframes) to the same moment.  Going back to full
        /// quality restores the remembered start exactly when the proxy start
        /// was not edited meanwhile.
        /// </summary>
        private static bool Apply(Swap s, bool toProxy)
        {
            OsvMedia m = s.Media;
            MediaRecord r = s.Record;
            int start = m.StartFrame;
            long mapped;
            if (toProxy)
            {
                r.FullStartFrame = start;
                mapped = ProxyTiming.MapStartFrame(start, s.FromProbe, s.ToProbe);
                r.ProxyStartFrame = mapped;
                r.Source = s.From;
                r.Proxy = s.To;
                r.OnProxy = true;
            }
            else
            {
                bool untouched = r.OnProxy && r.ProxyStartFrame == start;
                mapped = untouched ? r.FullStartFrame : ProxyTiming.MapStartFrame(start, s.FromProbe, s.ToProbe);
                r.Source = s.To;
                r.Proxy = s.From;
                r.OnProxy = false;
                r.FullStartFrame = mapped;
                r.ProxyStartFrame = -1;
            }
            if (!Ofx.SetString(m.Fx, SourceParams.File, s.To))
            {
                return false;
            }
            Ofx.SetInt(m.Fx, SourceParams.StartFrame, (int)Math.Min(int.MaxValue, mapped));
            MapStartFrameKeys(m, s.FromProbe, s.ToProbe);
            m.SaveRecord(r);
            Log.Info("proxy: '" + m.DisplayName + "' -> " + ProbeResult.FileNameOf(s.To) + ", start frame " + start + " -> " + mapped);
            return true;
        }

        /// <summary>A keyframed Start Frame (rare, but legal) gets every key mapped too.</summary>
        private static void MapStartFrameKeys(OsvMedia m, ProbeResult from, ProbeResult to)
        {
            try
            {
                if (!(Ofx.Find(m.Fx, SourceParams.StartFrame) is OFXIntegerParameter p) || !p.IsAnimated)
                {
                    return;
                }
                foreach (OFXIntegerKeyframe k in p.Keyframes)
                {
                    k.Value = (int)Math.Min(int.MaxValue, ProxyTiming.MapStartFrame(k.Value, from, to));
                }
                p.ParameterChanged();
            }
            catch (Exception ex)
            {
                Log.Warn("proxy: mapping Start Frame keyframes failed", ex);
            }
        }
    }
}
