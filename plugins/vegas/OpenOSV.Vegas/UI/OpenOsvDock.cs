// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OpenOsvDock.cs - the "OpenOSV" dock panel (View > Extensions > OpenOSV).
//
// ===========================================================================
//  Design, after Apple's Human Interface Guidelines
// ===========================================================================
//  Clarity    one primary action (Import OSV..., filled), everything else
//             tinted or grey; one column of inset grouped cards with small
//             section headers; numbers and names in the clip list, never
//             jargon.
//  Deference  VEGAS's own skin colours (Theme.cs), Segoe UI Variable, an
//             8-point grid; the panel looks like part of VEGAS, not a guest.
//  Depth      cards lift a step off the panel; the segmented thumb and switch
//             knob carry a soft shadow; motion (critically damped springs)
//             explains every state change - and stops entirely when Windows'
//             "Animation effects" are off.
//
//  Layout     ~260 px docked narrow: one column, labels above their controls.
//             300 px+: labels beside controls.  620 px+: two columns of cards.
//             Everything is measured in points and scaled by the monitor's DPI.
//
// The panel never talks to VEGAS on its own initiative except to READ: it
// refreshes (debounced) on project, media-pool, selection and project-
// property events, and every change it makes is a user action in one undo
// step, run through Guard.

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Windows.Forms;
using OpenOSV.Vegas.Actions;
using OpenOSV.Vegas.Core;
using OpenOSV.Vegas.Host;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.UI
{
    /// <summary>
    /// The dock window VEGAS hosts: a thin DockableControl around
    /// <see cref="OpenOsvPanel"/>.  The panel itself is a plain UserControl,
    /// so everything it draws can be built and rendered without VEGAS running
    /// (the scripting API's DockableControl needs a live VEGAS to construct).
    /// </summary>
    internal sealed class OpenOsvDock : DockableControl
    {
        private readonly OpenOsvPanel _panel;

        public OpenOsvDock() : base(OpenOsvModule.DockName)
        {
            DisplayName = "OpenOSV";
            try
            {
                DefaultFloatingSize = new Size(340, 760);
            }
            catch (Exception)
            {
                // Older VEGAS: VEGAS picks the size.
            }
            BackColor = Theme.Current.Background;
            _panel = new OpenOsvPanel { Dock = DockStyle.Fill };
            Controls.Add(_panel);
            Theme.Changed += OnThemeChanged;
        }

        private void OnThemeChanged() => Guard.Quietly("theming the dock", () => BackColor = Theme.Current.Background);

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Theme.Changed -= OnThemeChanged;
            }
            base.Dispose(disposing);
        }
    }

    /// <summary>The OpenOSV panel's content (hosted by <see cref="OpenOsvDock"/>).</summary>
    internal sealed class OpenOsvPanel : UserControl
    {
        // ---- structure -------------------------------------------------------------------
        private readonly ScrollHost _scroll;
        private readonly Panel _content;
        private readonly HeaderView _header;
        private readonly Banner _banner;
        private readonly StatusBar _status;
        private readonly Timer _refreshTimer;
        private readonly ToolTip _tips;

        // ---- import -------------------------------------------------------------------------------
        private readonly Card _importCard;
        private readonly OsvButton _importButton;
        private readonly DropZone _drop;

        // ---- clips -----------------------------------------------------------------------------------
        private readonly Card _clipsCard;
        private readonly ClipList _list;
        private readonly TextBlock _proxyLabel;
        private readonly OsvSegmented _proxy;

        // ---- the selected clip ---------------------------------------------------------------------
        private readonly Card _clipCard;
        private readonly TextBlock _clipName;
        private readonly TextBlock _clipMeta;
        private readonly TextBlock _outputLabel, _colourLabel, _levelsLabel, _rockLabel, _horizonLabel, _startLabel;
        private readonly OsvSegmented _output;
        private readonly OsvDropdown _colour;
        private readonly OsvSegmented _levels;
        private readonly TextBlock _levelsHint;
        private readonly OsvButton _matchLevels;
        private readonly OsvSwitch _rock;
        private readonly OsvSwitch _horizon;
        private readonly TextBlock _stabCaption;
        private readonly OsvStepper _start;
        private readonly TextBlock _startHint;

        // ---- looks, easing, tools, help ------------------------------------------------------------------
        private readonly Card _looksCard;
        private readonly List<OsvButton> _lookChips = new List<OsvButton>();
        private readonly TextBlock _looksCaption;
        private readonly Card _easingCard;
        private readonly List<EasingTile> _tiles = new List<EasingTile>();
        private readonly Card _toolsCard;
        private readonly List<OsvButton> _tools = new List<OsvButton>();
        private readonly Card _helpCard;
        private readonly TextBlock _helpText;
        private readonly TextBlock _toolPath;
        private readonly OsvButton _chooseTool;
        private readonly OsvButton _openLog;

        // ---- state ------------------------------------------------------------------------------------------
        private bool _subscribed;
        private bool _laying;
        private string _levelsNote = string.Empty;
        // Wanted visibility, kept apart from Control.Visible (which reads false
        // whenever a parent is hidden, e.g. before the dock is first shown).
        private bool _showMatch;
        private bool _showBanner;

        public OpenOsvPanel()
        {
            SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);
            BackColor = Theme.Current.Background;
            Font = Typeface.Of(TextStyle.Body);
            _tips = new ToolTip { InitialDelay = 500, ReshowDelay = 200 };

            // ---- the scroller, the content, the fixed status bar -------------------------------
            _status = new StatusBar { Dock = DockStyle.Bottom };
            _content = new Panel { Location = Point.Empty, BackColor = Theme.Current.Background };
            _scroll = new ScrollHost(_content) { Dock = DockStyle.Fill };
            Controls.Add(_scroll);
            Controls.Add(_status);

            _header = new HeaderView();
            _banner = new Banner { Visible = false };
            _banner.Action.Click += Guard.Click("Fix", () => BannerAction());
            _content.Controls.Add(_header);
            _content.Controls.Add(_banner);

            // ---- Import ----------------------------------------------------------------------------------
            _importCard = AddCard("Import");
            _importButton = _importCard.Add(new OsvButton("Import OSV...", ButtonStyle.Primary, Icons.Add));
            _importButton.Click += Guard.Click("Import OSV", () => ImportAction.RunWithDialog());
            _drop = _importCard.Add(new DropZone());
            _drop.Click += Guard.Click("Import OSV", () => ImportAction.RunWithDialog());
            _drop.FilesDropped += files => Guard.Run("Import OSV", () => ImportAction.Run(files));
            _importCard.Arrange = ArrangeImport;

            // ---- Clips ------------------------------------------------------------------------------------
            _clipsCard = AddCard("Clips in this project");
            _list = _clipsCard.Add(new ClipList());
            _list.SelectionChanged += (s, e) => Guard.Quietly("showing the selected clip", ShowSelectedClip);
            _proxyLabel = _clipsCard.Add(new TextBlock(TextStyle.Body, false) { Content = "Playback" });
            _proxy = _clipsCard.Add(new OsvSegmented("Full quality", "LRF proxies"));
            _proxy.SelectedIndexChanged += Guard.Click("Proxies", () =>
            {
                Guard.Report(ProxyAction.Run(_proxy.SelectedIndex == 1));
            });
            _tips.SetToolTip(_proxy, "Swap every OpenOSV clip between its .OSV and the .LRF proxy beside it. Timing stays exact.");
            _clipsCard.Arrange = ArrangeClips;

            // ---- The selected clip ------------------------------------------------------------------------
            _clipCard = AddCard("Selected clip");
            _clipName = _clipCard.Add(new TextBlock(TextStyle.BodyStrong, false));
            _clipMeta = _clipCard.Add(new TextBlock(TextStyle.Caption));
            _outputLabel = _clipCard.Add(Label("Output"));
            _output = _clipCard.Add(new OsvSegmented("Reframed", "360"));
            _output.SelectedIndexChanged += Guard.Click("Output", () => EditClip("Output", (p, m) => ClipEdit.SetOutput(p, m, _output.SelectedIndex)));
            _tips.SetToolTip(_output, "Reframed: a virtual camera at the project's size. 360: the whole sphere, 2:1.");
            _colourLabel = _clipCard.Add(Label("Colour"));
            _colour = _clipCard.Add(new OsvDropdown(ItemsOf(Choices.ColorOutput)));
            _colour.SelectedIndexChanged += Guard.Click("Colour", () =>
                EditClip("Colour", (p, m) => Ofx.SetChoice(m.Fx, StitchParams.ColorOutput, _colour.SelectedIndex, Choices.ColorOutput)));
            _levelsLabel = _clipCard.Add(Label("Levels"));
            _levels = _clipCard.Add(new OsvSegmented("Full", "Studio"));
            _levels.SelectedIndexChanged += Guard.Click("Levels", () =>
                EditClip("Levels", (p, m) => Ofx.SetChoice(m.Fx, SourceParams.OutputLevels, _levels.SelectedIndex, Choices.OutputLevels)));
            _tips.SetToolTip(_levels, "Full range (0-255) for 32-bit full-range projects; Studio RGB (16-235) for video-levels projects.");
            _levelsHint = _clipCard.Add(new TextBlock(TextStyle.Caption));
            _matchLevels = _clipCard.Add(new OsvButton("Match project", ButtonStyle.Tinted));
            _matchLevels.Click += Guard.Click("Match levels", () => Guard.Report(ClipEdit.MatchLevels()));
            _rockLabel = _clipCard.Add(Label("RockSteady"));
            _rock = _clipCard.Add(new OsvSwitch());
            _rock.CheckedChanged += Guard.Click("Stabilisation", ApplyStabilisationSwitches);
            _horizonLabel = _clipCard.Add(Label("Horizon Leveling"));
            _horizon = _clipCard.Add(new OsvSwitch());
            _horizon.CheckedChanged += Guard.Click("Stabilisation", ApplyStabilisationSwitches);
            _stabCaption = _clipCard.Add(new TextBlock(TextStyle.Caption));
            _startLabel = _clipCard.Add(Label("Start frame"));
            _start = _clipCard.Add(new OsvStepper { Minimum = 0, Maximum = 10_000_000 });
            _start.ValueChanged += Guard.Click("Start frame", () =>
                EditClip("Start frame", (p, m) => SetStartFrame(m, _start.Value)));
            _startHint = _clipCard.Add(new TextBlock(TextStyle.Caption));
            _clipCard.Arrange = ArrangeClip;

            // ---- Framing looks ----------------------------------------------------------------------------------
            _looksCard = AddCard("Framing");
            foreach (FramingLook look in DjiCamera.Looks)
            {
                string id = look.Id;
                OsvButton chip = _looksCard.Add(new OsvButton(look.Label, ButtonStyle.Chip));
                chip.Click += Guard.Click(look.Label, () =>
                {
                    Guard.Report(CameraActions.ApplyLook(id, SelectedMedia()));
                    MarkChosenLook(id);
                });
                _lookChips.Add(chip);
            }
            _looksCaption = _looksCard.Add(new TextBlock(TextStyle.Caption)
            {
                Content = "DJI Studio's looks, on the selected events - or the clip above when nothing is selected. Keyframed controls get a key at the playhead.",
            });
            _looksCard.Arrange = ArrangeLooks;

            // ---- Keyframe easing ---------------------------------------------------------------------------------
            _easingCard = AddCard("Keyframe animation");
            foreach (EasingPreset preset in DjiCamera.Easings)
            {
                string id = preset.Id;
                EasingTile tile = _easingCard.Add(new EasingTile(preset));
                tile.Click += Guard.Click(preset.Label, () =>
                {
                    Guard.Report(CameraActions.ApplyEasing(id, SelectedMedia()));
                    MarkChosenEasing(id);
                });
                _tips.SetToolTip(tile, preset.Label);
                _tiles.Add(tile);
            }
            _easingCard.Arrange = ArrangeEasing;

            // ---- Tools --------------------------------------------------------------------------------------------------
            _toolsCard = AddCard("Tools");
            AddTool("360 project setup", Icons.Globe, "A 2:1 frame, VEGAS's 360 output on, OSV clips as full spheres.",
                    () => Guard.Report(ProjectActions.Setup360()));
            AddTool("Apply 360 Reframe", Icons.Filter, "OpenOSV 360 Reframe on the selected non-OSV events, stretched to fill the frame.",
                    () => Guard.Report(ProjectActions.ApplyReframeFilter()));
            AddTool("Make framing unique", Icons.Copy, "Give each selected OSV event its own camera. Events cut from one clip share one otherwise.",
                    () => Guard.Report(MediaActions.MakeFramingUnique()));
            AddTool("Relink moved OSVs", Icons.Link, "Find clips that moved on disk, by file name, under a folder you pick.",
                    () => Guard.Report(MediaActions.Relink()));
            _toolsCard.Arrange = ArrangeTools;

            // ---- How it works (folds) ---------------------------------------------------------------------------------
            _helpCard = AddCard("How it works", true);
            _helpCard.Expanded = Session.Settings.HelpOpen;
            _helpCard.ExpandedChanged += (s, e) => Guard.Quietly("remembering the help card", () =>
            {
                Session.Settings.HelpOpen = _helpCard.Expanded;
                Session.SaveSettings();
            });
            _helpCard.HeightAnimating += (s, e) => Relayout();
            _helpText = _helpCard.Add(new TextBlock(TextStyle.Caption) { Content = HelpText() });
            _toolPath = _helpCard.Add(new TextBlock(TextStyle.Micro));
            _chooseTool = _helpCard.Add(new OsvButton("Choose osvtool...", ButtonStyle.Gray, Icons.Settings));
            _chooseTool.Click += Guard.Click("Choose osvtool", ChooseOsvTool);
            _openLog = _helpCard.Add(new OsvButton("Open the log", ButtonStyle.Gray, Icons.Document));
            _openLog.Click += Guard.Click("Open the log", OpenLog);
            _helpCard.Arrange = ArrangeHelp;

            // ---- refresh machinery -------------------------------------------------------------------------------------
            _refreshTimer = new Timer { Interval = 200 };
            _refreshTimer.Tick += (s, e) =>
            {
                _refreshTimer.Stop();
                Guard.Quietly("refreshing the OpenOSV panel", RefreshAll);
            };
            _scroll.Resize += (s, e) => Relayout();
            Theme.Changed += OnThemeChanged;
            Notifier.StatusChanged += OnStatus;
            Notifier.ClipsChanged += ScheduleRefresh;
        }

        // =====================================================================
        //  Lifetime
        // =====================================================================

        protected override void OnLoad(EventArgs e)
        {
            base.OnLoad(e);
            Guard.Quietly("starting the OpenOSV panel", () =>
            {
                Subscribe();
                Theme.Refresh();
                RefreshAll();
                MarkChosenLook(Session.Settings.LastLook);
                MarkChosenEasing(Session.Settings.LastEasing);
                _status.Show(Copy.Tagline, StatusKind.Info);
            });
        }

        /// <summary>Moved to a monitor with another DPI: every metric changes, so lay out again.</summary>
        protected override void OnDpiChangedAfterParent(EventArgs e)
        {
            base.OnDpiChangedAfterParent(e);
            Guard.Quietly("a DPI change", Relayout);
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                Guard.Quietly("closing the OpenOSV panel", () =>
                {
                    Unsubscribe();
                    Theme.Changed -= OnThemeChanged;
                    Notifier.StatusChanged -= OnStatus;
                    Notifier.ClipsChanged -= ScheduleRefresh;
                    _refreshTimer.Dispose();
                    _tips.Dispose();
                });
            }
            base.Dispose(disposing);
        }

        /// <summary>A middle dot between metadata items (" - " would read as a range).</summary>
        private static readonly string Dot = " " + (char)0x00B7 + " ";

        /// <summary>The VEGAS events that mean "what the panel shows may have changed".</summary>
        private static readonly string[] RefreshEvents =
        {
            "ProjectOpened", "ProjectClosed", "MediaPoolChanged", "TrackEventStateChanged",
            "TrackEventCountChanged", "ProjectPropsChanged",
        };

        /// <summary>
        /// Listen to VEGAS (read-only: every handler just schedules a refresh).
        /// Events are attached BY NAME: VEGAS 16 has no ProjectPropsChanged,
        /// and a compiled reference to it would fail this whole method there.
        /// A missing event is skipped; the others still arrive.
        /// </summary>
        private void Subscribe()
        {
            ScriptPortal.Vegas.Vegas app = VegasHost.App;
            if (app is null || _subscribed)
            {
                return;
            }
            foreach (string name in RefreshEvents)
            {
                VegasHost.TryAttach(app, name, (EventHandler)OnVegasChanged, true);
            }
            VegasHost.TryAttach(app, "AppSkinChanged", (EventHandler)OnSkinChanged, true);
            _subscribed = true;
        }

        private void Unsubscribe()
        {
            ScriptPortal.Vegas.Vegas app = VegasHost.App;
            if (app is null || !_subscribed)
            {
                return;
            }
            foreach (string name in RefreshEvents)
            {
                VegasHost.TryAttach(app, name, (EventHandler)OnVegasChanged, false);
            }
            VegasHost.TryAttach(app, "AppSkinChanged", (EventHandler)OnSkinChanged, false);
            _subscribed = false;
        }

        private void OnVegasChanged(object sender, EventArgs e) => Guard.Quietly("a VEGAS event", ScheduleRefresh);

        private void OnSkinChanged(object sender, EventArgs e) => Guard.Quietly("a skin change", Theme.Refresh);

        /// <summary>Coalesce a storm of VEGAS events into one refresh 200 ms after the last.</summary>
        private void ScheduleRefresh()
        {
            try
            {
                if (IsDisposed)
                {
                    return;
                }
                _refreshTimer.Stop();
                _refreshTimer.Start();
            }
            catch (Exception)
            {
                // Torn down between the check and the start.
            }
        }

        private void OnStatus(string text, StatusKind kind)
        {
            Guard.Quietly("showing a status line", () =>
            {
                _status.Show(text, kind);
                _tips.SetToolTip(_status, text);
            });
        }

        private void OnThemeChanged()
        {
            Guard.Quietly("applying the theme", () =>
            {
                BackColor = Theme.Current.Background;
                _scroll.Invalidate(true);
                _content.BackColor = Theme.Current.Background;
                foreach (Control c in _content.Controls)
                {
                    c.Invalidate(true);
                }
                _status.Invalidate();
            });
        }

        // =====================================================================
        //  Reading the project
        // =====================================================================

        /// <summary>Re-read everything the panel shows.</summary>
        private void RefreshAll()
        {
            Project project = VegasHost.Project;
            UpdateBanner();

            // ---- the clip list ------------------------------------------------------------------
            List<OsvMedia> media = OsvMedia.AllIn(project);
            Dictionary<string, int> eventCounts = Timeline.EventCountsByMediaKey(project);
            var rows = new List<ClipRow>(media.Count);
            foreach (OsvMedia m in media)
            {
                Size size = m.FrameSize;
                bool equirect = m.Output == Choices.OutputEquirect;
                string length = TimeMath.FormatDuration(VegasHost.Seconds(m.Length));
                rows.Add(new ClipRow
                {
                    Key = m.Key,
                    Name = m.DisplayName,
                    Detail = (equirect ? "360" : "Reframed") + Dot + size.Width + " x " + size.Height + Dot + length,
                    Proxy = m.OnProxy,
                    Offline = m.IsOffline,
                    Equirect = equirect,
                    EventCount = eventCounts.TryGetValue(m.Key, out int events) ? events : 0,
                });
            }
            _list.SetRows(rows);

            // ---- follow the timeline: one OSV clip selected there selects it here ------------------------
            string timelineKey = null;
            foreach (VideoEvent evt in Timeline.SelectedVideoEvents(project))
            {
                OsvMedia m = OsvMedia.ForEvent(evt);
                if (m is null)
                {
                    continue;
                }
                if (timelineKey is null)
                {
                    timelineKey = m.Key;
                }
                else if (timelineKey != m.Key)
                {
                    timelineKey = null;
                    break;
                }
            }
            if (timelineKey is not null)
            {
                _list.SelectKey(timelineKey);
            }

            // ---- the proxy switch reflects the project ------------------------------------------------------
            bool? proxyState = ProxyAction.State(project);
            _proxy.SelectedIndex = proxyState == true ? 1 : (proxyState == false ? 0 : -1);
            _proxy.Enabled = media.Count > 0;

            ShowSelectedClip();
            Relayout();
        }

        /// <summary>The media behind the selected row, looked up fresh (VEGAS objects are never cached).</summary>
        private OsvMedia SelectedMedia()
        {
            string key = _list.SelectedKey;
            if (key is null)
            {
                return null;
            }
            foreach (OsvMedia m in OsvMedia.AllIn(VegasHost.Project))
            {
                if (m.Key == key)
                {
                    return m;
                }
            }
            return null;
        }

        /// <summary>Show the selected clip's controls (from code: no events fire).</summary>
        private void ShowSelectedClip()
        {
            OsvMedia m = SelectedMedia();
            bool has = m is not null;
            foreach (Control c in new Control[] { _output, _colour, _levels, _rock, _horizon, _start, _matchLevels })
            {
                c.Enabled = has;
            }
            if (!has)
            {
                _clipName.Content = "No clip selected";
                _clipMeta.PathLine = false;
                _clipMeta.Tone = TextTone.Default;
                _clipMeta.Content = "Import a clip, or select an OSV event on the timeline.";
                _tips.SetToolTip(_clipMeta, null);
                _levelsHint.Content = string.Empty;
                _stabCaption.Content = string.Empty;
                _startHint.Content = string.Empty;
                Relayout();
                return;
            }

            _clipName.Content = m.DisplayName;
            string file = m.FilePath;
            // One line, shortened in the middle: a path has no spaces to wrap at.
            _clipMeta.PathLine = !m.IsOffline;
            _clipMeta.Content = m.IsOffline
                ? "Offline: " + (string.IsNullOrEmpty(file) ? "no file set" : ProbeResult.FileNameOf(file)) + " is gone. Try Relink moved OSVs."
                : (m.OnProxy ? "Proxy: " : "Full quality: ") + file;
            _clipMeta.Tone = m.IsOffline ? TextTone.Error : TextTone.Default;
            _tips.SetToolTip(_clipMeta, file);

            _output.SelectedIndex = m.Output;
            _colour.SelectedIndex = m.ColorOutput;

            // ---- levels: the value, and whether it suits this project -------------------------------------
            int levels = m.OutputLevels;
            Project project = VegasHost.Project;
            ProjectLevels projectLevels = VegasHost.Levels(project);
            if (levels < 0)
            {
                _levels.SelectedIndex = -1;
                _levels.Enabled = false;
                _levelsNote = "This OpenFX bundle has no Output Levels control yet. Update it for level-correct output in VEGAS.";
                _showMatch = false;
            }
            else
            {
                _levels.SelectedIndex = levels;
                int wanted = projectLevels == ProjectLevels.Full ? Choices.LevelsFull : Choices.LevelsStudio;
                string pixelFormat = ProjectColour.Describe(VegasHost.PixelFormatName(project));
                if (levels != wanted)
                {
                    _levelsNote = Copy.LevelsMismatch + " The project is " + pixelFormat + ".";
                    _showMatch = true;
                }
                else
                {
                    _levelsNote = "Matches the project (" + pixelFormat + ").";
                    _showMatch = false;
                }
            }
            _levelsHint.Content = _levelsNote;
            _levelsHint.Tone = _showMatch ? TextTone.Warning : TextTone.Default;

            // ---- stabilisation: the two switches spell every entry but Full ------------------------------------
            int stab = m.Stabilization;
            bool spelled = Stabilisation.TrySwitchesFor(stab, out bool rs, out bool hl);
            _rock.Checked = rs;
            _horizon.Checked = hl;
            _stabCaption.Content = spelled
                ? "From the camera's gyro: " + Stabilisation.Caption(stab) + "."
                : "Set to Full (locked to the first frame's heading). Flip a switch to replace it.";

            // ---- start frame: in the file's own frames; editable on full quality only ------------------------------
            _start.Value = m.StartFrame;
            _start.Enabled = !m.OnProxy;
            _startHint.Content = m.OnProxy ? "Switch to full quality to change the start frame." : "The clip frame shown on the event's first frame.";
            Relayout();
        }

        /// <summary>Show what is missing: the OpenFX bundle, osvtool.</summary>
        private void UpdateBanner()
        {
            string message = null;
            string action = null;
            StatusKind kind = StatusKind.Warning;
            if (VegasHost.FindSourceGenerator() is null)
            {
                message = Copy.NoGenerator;
                kind = StatusKind.Error;
            }
            else if (!Session.ToolLocation.Found)
            {
                message = "osvtool.exe wasn't found, so clips can't be imported yet. It ships in the OpenOSV OpenFX bundle.";
                action = "Choose osvtool...";
            }
            _showBanner = message is not null;
            _banner.Visible = _showBanner;
            _banner.Kind = kind;
            _banner.Message = message ?? string.Empty;
            _banner.Action.Text = action ?? string.Empty;
            _header.ToolFound = Session.ToolLocation.Found;
            _toolPath.Content = Session.ToolLocation.Found ? "osvtool: " + Session.ToolLocation.ExePath : "osvtool: not found";
        }

        private void BannerAction()
        {
            if (!Session.ToolLocation.Found)
            {
                ChooseOsvTool();
            }
        }

        // =====================================================================
        //  User actions
        // =====================================================================

        /// <summary>One edit of the selected clip, in its own undo step.</summary>
        private void EditClip(string what, Func<Project, OsvMedia, bool> edit)
        {
            OsvMedia m = SelectedMedia();
            if (m is null)
            {
                Notifier.Status("Select a clip first.", StatusKind.Warning);
                return;
            }
            Guard.Report(ClipEdit.Edit(m, what, edit));
        }

        private static bool SetStartFrame(OsvMedia m, int value)
        {
            if (m.OnProxy)
            {
                return false;
            }
            bool ok = Ofx.SetInt(m.Fx, SourceParams.StartFrame, value);
            if (ok)
            {
                MediaRecord r = m.RecordOrDerived();
                r.FullStartFrame = value;
                m.SaveRecord(r);
            }
            return ok;
        }

        private void ApplyStabilisationSwitches()
        {
            // The switches act on the selected events - or this clip when none are.
            Guard.Report(CameraActions.ApplyStabilisation(_rock.Checked, _horizon.Checked, SelectedMedia()));
        }

        private void MarkChosenLook(string id)
        {
            for (int i = 0; i < _lookChips.Count && i < DjiCamera.Looks.Count; ++i)
            {
                _lookChips[i].Selected = string.Equals(DjiCamera.Looks[i].Id, id, StringComparison.OrdinalIgnoreCase);
            }
        }

        private void MarkChosenEasing(string id)
        {
            foreach (EasingTile tile in _tiles)
            {
                tile.Selected = string.Equals(tile.Preset.Id, id, StringComparison.OrdinalIgnoreCase);
            }
        }

        private void ChooseOsvTool()
        {
            using (var dialog = new OpenFileDialog
            {
                Title = "Where is osvtool.exe?",
                Filter = "osvtool|osvtool.exe|Programs (*.exe)|*.exe",
                CheckFileExists = true,
            })
            {
                if (dialog.ShowDialog(Guard.Owner) != DialogResult.OK)
                {
                    return;
                }
                Session.Settings.OsvToolPath = dialog.FileName;
                Session.SaveSettings();
                Session.ForgetTool();
            }
            OsvTool tool = Session.Tool;
            string version = tool?.Version(default(System.Threading.CancellationToken));
            Notifier.Status(tool is null ? "That file isn't a usable osvtool." : "Using " + (version ?? "osvtool") + ".",
                            tool is null ? StatusKind.Error : StatusKind.Success);
            RefreshAll();
        }

        private static void OpenLog()
        {
            string log = Log.FilePath;
            string dir = Path.GetDirectoryName(log);
            if (File.Exists(log))
            {
                Process.Start("explorer.exe", "/select,\"" + log + "\"");
            }
            else if (!string.IsNullOrEmpty(dir) && AppPaths.EnsureDir(dir))
            {
                Process.Start("explorer.exe", "\"" + dir + "\"");
            }
        }

        private static string HelpText()
        {
            return "VEGAS can't open .OSV files, so OpenOSV Source (an OpenFX generator) plays them, and this panel does the setup VEGAS " +
                   "can't: the exact length, the frame size, the audio (a sync-exact WAV, grouped with the clip) and the levels your project " +
                   "works in.\n\n" +
                   "A generator keeps its settings on the MEDIA: every event cut from one clip shares its camera. Make framing unique " +
                   "gives an event its own.\n\n" +
                   "Other 360 footage: select its events and Apply 360 Reframe. The sphere is stretched to fill the frame, on purpose.\n\n" +
                   Copy.HdrNote;
        }

        // =====================================================================
        //  Layout
        // =====================================================================

        private Card AddCard(string title, bool collapsible = false)
        {
            var card = new Card(title, collapsible);
            _content.Controls.Add(card);
            return card;
        }

        private static TextBlock Label(string text) => new TextBlock(TextStyle.Body, false) { Content = text };

        private static string[] ItemsOf(ChoiceList list)
        {
            var items = new string[list.Count];
            for (int i = 0; i < list.Count; ++i)
            {
                items[i] = list[i];
            }
            return items;
        }

        private void AddTool(string text, char glyph, string tip, Action body)
        {
            OsvButton b = _toolsCard.Add(new OsvButton(text, ButtonStyle.Gray, glyph));
            b.Click += Guard.Click(text, body);
            _tips.SetToolTip(b, tip);
            _tools.Add(b);
        }

        private float DpiScale => DeviceDpi > 0 ? DeviceDpi / 96f : 1f;

        private int Px(float v) => (int)Math.Round(v * DpiScale);

        /// <summary>True when labels sit beside their controls.</summary>
        private bool SideBySide(int width) => width >= Px(276);

        /// <summary>Lay every card out for the current width.</summary>
        private void Relayout()
        {
            if (_laying || IsDisposed)
            {
                return;
            }
            _laying = true;
            try
            {
                SuspendLayout();
                int margin = Px(12);
                int gap = Px(16);
                // The overlay scroller takes no width: the cards use all of it
                // (and there is never a horizontal scroll bar).
                int width = Math.Max(Px(200), _scroll.ClientSize.Width);
                int inner = width - 2 * margin;
                int y = margin;

                // ---- header and banner, full width -----------------------------------------------
                int headerH = _header.HeightFor(inner);
                _header.SetBounds(margin, y, inner, headerH);
                y += headerH + Px(8);
                if (_showBanner)
                {
                    int bh = _banner.LayoutFor(inner);
                    _banner.SetBounds(margin, y, inner, bh);
                    y += bh + gap;
                }

                // ---- one column, or two when wide ------------------------------------------------------
                var left = new Card[] { _importCard, _clipsCard, _clipCard };
                var right = new Card[] { _looksCard, _easingCard, _toolsCard, _helpCard };
                if (inner >= Px(620))
                {
                    int colW = (inner - gap) / 2;
                    int yl = PlaceColumn(left, margin, y, colW, gap);
                    int yr = PlaceColumn(right, margin + colW + gap, y, colW, gap);
                    y = Math.Max(yl, yr);
                }
                else
                {
                    y = PlaceColumn(left, margin, y, inner, gap);
                    y = PlaceColumn(right, margin, y, inner, gap);
                }
                _content.SetBounds(0, _content.Top, width, y + margin);
                _scroll.UpdateExtent();
                _status.Height = _status.PreferredHeight;
                ResumeLayout(true);
            }
            catch (Exception ex)
            {
                Log.Warn("ui: laying out the panel failed", ex);
            }
            finally
            {
                _laying = false;
            }
        }

        private static int PlaceColumn(Card[] cards, int x, int y, int width, int gap)
        {
            foreach (Card c in cards)
            {
                int h = c.LayoutFor(width);
                c.SetBounds(x, y, width, h);
                y += h + gap;
            }
            return y - gap;
        }

        // ---- per-card arrangers (content coordinates, return the content height) ----------------------------

        private int ArrangeImport(int w)
        {
            int y = 0;
            _importCard.Place(_importButton, 0, y, w, _importButton.PreferredHeight);
            y += _importButton.PreferredHeight + Px(8);
            _importCard.Place(_drop, 0, y, w, _drop.PreferredHeight);
            return y + _drop.PreferredHeight;
        }

        private int ArrangeClips(int w)
        {
            int y = 0;
            _clipsCard.Place(_list, 0, y, w, _list.PreferredHeight);
            y += _list.PreferredHeight + Px(12);
            y = Row(_clipsCard, _proxyLabel, _proxy, _proxy.PreferredHeight, w, y);
            return y;
        }

        private int ArrangeClip(int w)
        {
            int y = 0;
            int nameH = Math.Max(Px(18), _clipName.HeightFor(w));
            _clipCard.Place(_clipName, 0, y, w, nameH);
            y += nameH;
            int metaH = _clipMeta.HeightFor(w);
            _clipCard.Place(_clipMeta, 0, y, w, Math.Max(1, metaH), metaH > 0);
            y += metaH + Px(12);

            y = Row(_clipCard, _outputLabel, _output, _output.PreferredHeight, w, y) + Px(8);
            y = Row(_clipCard, _colourLabel, _colour, _colour.PreferredHeight, w, y) + Px(8);
            y = Row(_clipCard, _levelsLabel, _levels, _levels.PreferredHeight, w, y);
            int hintH = _levelsHint.HeightFor(w);
            _clipCard.Place(_levelsHint, 0, y + Px(4), w, Math.Max(1, hintH), hintH > 0);
            y += hintH + Px(4);
            if (_showMatch)
            {
                int bw = Math.Min(w, _matchLevels.PreferredContentWidth);
                _clipCard.Place(_matchLevels, 0, y + Px(4), bw, _matchLevels.PreferredHeight, true);
                y += _matchLevels.PreferredHeight + Px(4);
            }
            else
            {
                _matchLevels.Visible = false;
            }
            y += Px(12);

            // Switches always sit beside their labels (the platform's rule).
            y = SwitchRow(_rockLabel, _rock, w, y);
            y = SwitchRow(_horizonLabel, _horizon, w, y);
            int capH = _stabCaption.HeightFor(w);
            _clipCard.Place(_stabCaption, 0, y, w, Math.Max(1, capH), capH > 0);
            y += capH + Px(12);

            y = Row(_clipCard, _startLabel, _start, _start.PreferredHeight, w, y);
            int startH = _startHint.HeightFor(w);
            _clipCard.Place(_startHint, 0, y + Px(4), w, Math.Max(1, startH), startH > 0);
            return y + Px(4) + startH;
        }

        private int ArrangeLooks(int w)
        {
            int y = Flow(_looksCard, _lookChips, w, 0, b => b.PreferredContentWidth, b => b.PreferredHeight, Px(8));
            int capH = _looksCaption.HeightFor(w);
            _looksCard.Place(_looksCaption, 0, y + Px(10), w, Math.Max(1, capH), capH > 0);
            return y + Px(10) + capH;
        }

        private int ArrangeEasing(int w)
        {
            if (_tiles.Count == 0)
            {
                return 0;
            }
            Size tile = _tiles[0].PreferredTileSize;
            int gap = Px(8);
            int columns = Math.Max(2, (w + gap) / (tile.Width + gap));
            int tileW = (w - (columns - 1) * gap) / columns;
            int y = 0;
            for (int i = 0; i < _tiles.Count; ++i)
            {
                int col = i % columns;
                int row = i / columns;
                y = row * (tile.Height + gap);
                _easingCard.Place(_tiles[i], col * (tileW + gap), y, tileW, tile.Height);
            }
            return y + tile.Height;
        }

        private int ArrangeTools(int w)
        {
            int gap = Px(8);
            // Two per row only when every label fits whole: a truncated
            // command name is worse than a taller card.
            int widest = 0;
            foreach (OsvButton b in _tools)
            {
                widest = Math.Max(widest, b.PreferredContentWidth);
            }
            bool two = (w - gap) / 2 >= widest;
            int bw = two ? (w - gap) / 2 : w;
            int y = 0;
            for (int i = 0; i < _tools.Count; ++i)
            {
                int col = two ? i % 2 : 0;
                int row = two ? i / 2 : i;
                y = row * (_tools[i].PreferredHeight + gap);
                _toolsCard.Place(_tools[i], col * (bw + gap), y, bw, _tools[i].PreferredHeight);
            }
            return y + (_tools.Count > 0 ? _tools[0].PreferredHeight : 0);
        }

        private int ArrangeHelp(int w)
        {
            int y = 0;
            int textH = _helpText.HeightFor(w);
            _helpCard.Place(_helpText, 0, y, w, textH);
            y += textH + Px(12);
            int pathH = _toolPath.HeightFor(w);
            _helpCard.Place(_toolPath, 0, y, w, Math.Max(1, pathH));
            y += pathH + Px(8);
            int gap = Px(8);
            bool two = w >= Px(300);
            int bw = two ? (w - gap) / 2 : w;
            _helpCard.Place(_chooseTool, 0, y, bw, _chooseTool.PreferredHeight);
            if (two)
            {
                _helpCard.Place(_openLog, bw + gap, y, bw, _openLog.PreferredHeight);
                return y + _chooseTool.PreferredHeight;
            }
            y += _chooseTool.PreferredHeight + gap;
            _helpCard.Place(_openLog, 0, y, bw, _openLog.PreferredHeight);
            return y + _openLog.PreferredHeight;
        }

        /// <summary>A label and its control: side by side when there is room, else stacked.</summary>
        private int Row(Card card, TextBlock label, Control control, int controlHeight, int w, int y)
        {
            if (SideBySide(w))
            {
                // The label's single line sits on the control's vertical centre.
                int labelW = Math.Min(Px(112), w / 3);
                int labelH = Px(20);
                card.Place(label, 0, y + Math.Max(0, (controlHeight - labelH) / 2), labelW, labelH);
                card.Place(control, labelW + Px(8), y, w - labelW - Px(8), controlHeight);
                return y + controlHeight;
            }
            int lh = Px(20);
            card.Place(label, 0, y, w, lh);
            card.Place(control, 0, y + lh, w, controlHeight);
            return y + lh + controlHeight;
        }

        private int SwitchRow(TextBlock label, OsvSwitch sw, int w, int y)
        {
            Size size = sw.PreferredSwitchSize;
            int h = Math.Max(size.Height, Px(28));
            _clipCard.Place(label, 0, y + (h - Px(20)) / 2, w - size.Width - Px(8), Px(20));
            _clipCard.Place(sw, w - size.Width - Px(4), y, size.Width + Px(4), h);
            return y + h + Px(4);
        }

        /// <summary>Flow controls left to right, wrapping; returns the height used.</summary>
        private static int Flow<T>(Card card, List<T> items, int w, int y, Func<T, int> width, Func<T, int> height, int gap) where T : Control
        {
            int x = 0;
            int rowH = 0;
            foreach (T item in items)
            {
                int iw = Math.Min(w, width(item));
                int ih = height(item);
                if (x > 0 && x + iw > w)
                {
                    x = 0;
                    y += rowH + gap;
                    rowH = 0;
                }
                card.Place(item, x, y, iw, ih);
                x += iw + gap;
                rowH = Math.Max(rowH, ih);
            }
            return y + rowH;
        }
    }

    /// <summary>The panel's title, tagline and the osvtool indicator.</summary>
    internal sealed class HeaderView : OsvControl
    {
        private bool _toolFound = true;

        /// <summary>Whether osvtool was found (the pill's state).</summary>
        public bool ToolFound
        {
            get => _toolFound;
            set
            {
                _toolFound = value;
                Invalidate();
            }
        }

        /// <summary>The header's height at a width (the tagline drops under ~280 px).</summary>
        public int HeightFor(int width) => width >= Px(280) ? Px(48) : Px(30);

        protected override void PaintContent(Graphics g)
        {
            Theme t = Theme.Current;
            string pill = _toolFound ? "Ready" : "No osvtool";
            Color pillColour = _toolFound ? t.Green : t.Orange;
            Size pillText = Draw.Measure(g, pill, TextStyle.Micro);
            int pillW = pillText.Width + Px(22);
            int pillH = Px(20);
            var pillRect = new Rectangle(Width - pillW, Px(4), pillW, pillH);
            Draw.Fill(g, pillRect, pillH / 2f, Theme.Alpha(pillColour, 0.16));
            using (var b = new SolidBrush(pillColour))
            {
                float d = 6f * S;
                g.FillEllipse(b, pillRect.X + Px(8), pillRect.Y + (pillH - d) / 2f, d, d);
            }
            Draw.Text(g, pill, TextStyle.Micro, new Rectangle(pillRect.X + Px(16), pillRect.Y, pillW - Px(18), pillH), pillColour,
                      TextFormatFlags.Left | TextFormatFlags.VerticalCenter);

            Draw.Text(g, Copy.ProductName, TextStyle.Title, new Rectangle(0, 0, Width - pillW - Px(8), Px(28)), t.Text);
            if (Height >= Px(40))
            {
                Draw.Text(g, Copy.Tagline, TextStyle.Caption, new Rectangle(0, Px(28), Width, Px(18)), t.SecondaryText);
            }
        }
    }
}
