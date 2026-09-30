// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// Dialogs.cs - the extension's two windows: a message and a progress sheet,
// both borderless, rounded (Windows 11) and in the panel's palette.

using System;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.Windows.Forms;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.UI
{
    /// <summary>The kind of a message.</summary>
    internal enum MessageKind
    {
        /// <summary>Neutral.</summary>
        Info,
        /// <summary>Something to check.</summary>
        Warning,
        /// <summary>Something failed.</summary>
        Error,
    }

    /// <summary>
    /// A borderless themed window: rounded corners and a shadow from the
    /// window manager, dragged by its top strip, closed with Esc.
    /// </summary>
    internal class OsvForm : Form
    {
        private const int CsDropShadow = 0x00020000;
        private Point _dragFrom;
        private bool _dragging;

        public OsvForm()
        {
            FormBorderStyle = FormBorderStyle.None;
            ShowInTaskbar = false;
            StartPosition = FormStartPosition.CenterParent;
            KeyPreview = true;
            DoubleBuffered = true;
            AutoScaleMode = AutoScaleMode.None;
            BackColor = Theme.Current.Card;
            Font = Typeface.Of(TextStyle.Body);
        }

        /// <summary>DPI scale.</summary>
        protected float S => DeviceDpi > 0 ? DeviceDpi / 96f : 1f;

        /// <summary>A length on the 8-point grid, in device pixels.</summary>
        protected int Px(float v) => (int)Math.Round(v * S);

        protected override CreateParams CreateParams
        {
            get
            {
                CreateParams cp = base.CreateParams;
                cp.ClassStyle |= CsDropShadow;
                return cp;
            }
        }

        [DllImport("dwmapi.dll")]
        private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);

        protected override void OnHandleCreated(EventArgs e)
        {
            base.OnHandleCreated(e);
            try
            {
                // Windows 11: DWMWA_WINDOW_CORNER_PREFERENCE (33) = DWMWCP_ROUND (2).
                // Older Windows ignores the attribute; square corners it is.
                int round = 2;
                DwmSetWindowAttribute(Handle, 33, ref round, sizeof(int));
            }
            catch (Exception)
            {
                // No DWM: square corners.
            }
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Escape)
            {
                OnEscape();
                e.Handled = true;
            }
            base.OnKeyDown(e);
        }

        /// <summary>Esc: close by default.</summary>
        protected virtual void OnEscape() => Close();

        protected override void OnMouseDown(MouseEventArgs e)
        {
            if (e.Button == MouseButtons.Left && e.Y < Px(44))
            {
                _dragging = true;
                _dragFrom = e.Location;
            }
            base.OnMouseDown(e);
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            if (_dragging)
            {
                Location = new Point(Location.X + e.X - _dragFrom.X, Location.Y + e.Y - _dragFrom.Y);
            }
            base.OnMouseMove(e);
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            _dragging = false;
            base.OnMouseUp(e);
        }

        protected override void OnPaintBackground(PaintEventArgs e)
        {
            using (var b = new SolidBrush(Theme.Current.Card))
            {
                e.Graphics.FillRectangle(b, ClientRectangle);
            }
            using (var pen = new Pen(Theme.Current.CardBorder, 1f))
            {
                e.Graphics.DrawRectangle(pen, 0, 0, Width - 1, Height - 1);
            }
        }

        /// <summary>Centre on the owner (or the screen) once the size is known.</summary>
        protected void CentreOn(IWin32Window owner)
        {
            try
            {
                Rectangle area = Screen.FromPoint(Cursor.Position).WorkingArea;
                if (owner is not null && owner.Handle != IntPtr.Zero)
                {
                    Control c = Control.FromHandle(owner.Handle);
                    Rectangle ownerBounds = c is not null ? c.RectangleToScreen(c.ClientRectangle) : GetWindowRect(owner.Handle);
                    if (ownerBounds.Width > 0)
                    {
                        area = ownerBounds;
                    }
                }
                StartPosition = FormStartPosition.Manual;
                Location = new Point(area.X + (area.Width - Width) / 2, area.Y + (area.Height - Height) / 3);
            }
            catch (Exception)
            {
                StartPosition = FormStartPosition.CenterScreen;
            }
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct RECT
        {
            public int Left, Top, Right, Bottom;
        }

        [DllImport("user32.dll")]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool GetWindowRect(IntPtr hwnd, out RECT rect);

        private static Rectangle GetWindowRect(IntPtr hwnd)
        {
            return GetWindowRect(hwnd, out RECT r) ? Rectangle.FromLTRB(r.Left, r.Top, r.Right, r.Bottom) : Rectangle.Empty;
        }
    }

    /// <summary>A themed message box: a title, the message, details that can be copied, OK.</summary>
    internal sealed class MessageDialog : OsvForm
    {
        private readonly string _title;
        private readonly string _summary;
        private readonly MessageKind _kind;
        private readonly TextBox _details;
        private readonly OsvButton _ok;
        private int _summaryHeight;

        private MessageDialog(string title, string text, MessageKind kind)
        {
            _title = string.IsNullOrWhiteSpace(title) ? Copy.ProductName : title;
            _kind = kind;
            // The first paragraph is the summary; the rest are details.
            string body = text ?? string.Empty;
            int split = body.IndexOf("\n", StringComparison.Ordinal);
            _summary = split > 0 ? body.Substring(0, split).Trim() : body.Trim();
            string details = split > 0 ? body.Substring(split + 1).Trim('\n', '\r', ' ') : string.Empty;

            Text = _title;
            int width = Px(440);
            int textWidth = width - Px(76);
            _summaryHeight = Draw.Measure(_summary, TextStyle.Body, textWidth, true).Height;
            int y = Px(52) + _summaryHeight + Px(12);

            // ---- details: a read-only box the user can select and copy from ---------------
            if (details.Length > 0)
            {
                _details = new TextBox
                {
                    Multiline = true,
                    ReadOnly = true,
                    BorderStyle = BorderStyle.None,
                    ScrollBars = ScrollBars.Vertical,
                    Text = details.Replace("\r\n", "\n").Replace("\n", "\r\n"),
                    BackColor = Theme.Mix(Theme.Current.Card, Theme.Current.Text, 0.05),
                    ForeColor = Theme.Current.SecondaryText,
                    Font = Typeface.Of(TextStyle.Caption),
                    TabStop = false,
                };
                int lines = Math.Min(12, Math.Max(2, _details.Lines.Length + 1));
                int detailsHeight = lines * (Typeface.Of(TextStyle.Caption).Height + 1);
                _details.SetBounds(Px(60), y, textWidth, detailsHeight);
                Controls.Add(_details);
                y += detailsHeight + Px(16);
            }

            // ---- OK -----------------------------------------------------------------------------
            _ok = new OsvButton("OK", ButtonStyle.Primary) { Surface = Theme.Current.Card };
            _ok.Click += (s, e) => Close();
            int bw = Px(96);
            _ok.SetBounds(width - bw - Px(16), y, bw, _ok.PreferredHeight);
            Controls.Add(_ok);
            y += _ok.PreferredHeight + Px(16);
            ClientSize = new Size(width, y);
            AcceptButton = null;
        }

        /// <summary>Show a message, modal to VEGAS.  Never throws.</summary>
        public static void Show(IWin32Window owner, string title, string text, MessageKind kind)
        {
            try
            {
                using (var dlg = new MessageDialog(title, text, kind))
                {
                    dlg.CentreOn(owner);
                    dlg.ShowDialog(owner);
                }
            }
            catch (Exception ex)
            {
                Log.Error("dialog: the themed message failed; falling back to a plain one", ex);
                try
                {
                    MessageBox.Show(owner, text, title, MessageBoxButtons.OK,
                                    kind == MessageKind.Error ? MessageBoxIcon.Error : (kind == MessageKind.Warning ? MessageBoxIcon.Warning : MessageBoxIcon.Information));
                }
                catch (Exception)
                {
                    // Nothing left to show it with; the log has it.
                }
            }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            _ok.Focus();
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (e.KeyCode == Keys.Enter && (_details is null || !_details.Focused))
            {
                Close();
                e.Handled = true;
                return;
            }
            base.OnKeyDown(e);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            try
            {
                Graphics g = e.Graphics;
                Draw.Prepare(g);
                Theme t = Theme.Current;
                Color tint = _kind == MessageKind.Error ? t.Red : (_kind == MessageKind.Warning ? t.Orange : t.Accent);
                char glyph = _kind == MessageKind.Error ? Icons.Error : (_kind == MessageKind.Warning ? Icons.Warning : Icons.Info);
                var badge = new Rectangle(Px(16), Px(16), Px(32), Px(32));
                Draw.Fill(g, badge, badge.Width / 2f, Theme.Alpha(tint, 0.18));
                Icons.Paint(g, glyph, badge, tint, 12f);
                Draw.Text(g, _title, TextStyle.BodyStrong, new Rectangle(Px(60), Px(16), Width - Px(76), Px(22)), t.Text);
                Draw.Text(g, _summary, TextStyle.Body, new Rectangle(Px(60), Px(42), Width - Px(76), _summaryHeight + Px(4)), t.Text,
                          TextFormatFlags.Left | TextFormatFlags.Top | TextFormatFlags.WordBreak);
            }
            catch (Exception ex)
            {
                Log.Warn("dialog: painting failed", ex);
            }
        }
    }

    /// <summary>What a background job reports: a fraction and a line.  Thread-safe.</summary>
    internal sealed class Progress
    {
        private readonly object _gate = new object();
        private double _fraction;
        private string _text = string.Empty;

        /// <summary>Set the progress (0..1) and the current line.</summary>
        public void Report(double fraction, string text)
        {
            lock (_gate)
            {
                _fraction = double.IsNaN(fraction) ? 0 : Math.Max(0, Math.Min(1, fraction));
                if (text is not null)
                {
                    _text = text;
                }
            }
        }

        /// <summary>Read both.</summary>
        public void Read(out double fraction, out string text)
        {
            lock (_gate)
            {
                fraction = _fraction;
                text = _text;
            }
        }
    }

    /// <summary>
    /// Runs slow work (osvtool) off VEGAS's UI thread behind a progress sheet
    /// with a Cancel button.  The VEGAS scripting API is only ever called from
    /// the UI thread - before and after, never inside the work.
    /// </summary>
    internal sealed class ProgressDialog : OsvForm
    {
        private readonly string _title;
        private readonly Progress _progress;
        private readonly CancellationTokenSource _cancel;
        private readonly Spring _bar = new Spring(0, 0.4);
        private readonly System.Windows.Forms.Timer _poll;
        private readonly OsvButton _cancelButton;
        private string _line = string.Empty;
        private bool _finished;

        private ProgressDialog(string title, Progress progress, CancellationTokenSource cancel)
        {
            _title = title;
            _progress = progress;
            _cancel = cancel;
            Text = title;
            ClientSize = new Size(Px(400), Px(132));
            _cancelButton = new OsvButton("Cancel", ButtonStyle.Gray) { Surface = Theme.Current.Card };
            _cancelButton.Click += (s, e) => RequestCancel();
            _cancelButton.SetBounds(ClientSize.Width - Px(112), ClientSize.Height - Px(48), Px(96), _cancelButton.PreferredHeight);
            Controls.Add(_cancelButton);
            // A 30 Hz poll reads the worker's progress: no cross-thread calls at all.
            _poll = new System.Windows.Forms.Timer { Interval = 33 };
            _poll.Tick += (s, e) => Poll();
        }

        /// <summary>
        /// Run <paramref name="work"/> on a pool thread.  Returns its result on
        /// the calling (UI) thread.  The sheet appears only if the work takes
        /// longer than a blink (350 ms), so a single cached probe never flashes
        /// a window.  An exception inside the work is logged and re-thrown here,
        /// where the caller's Guard catches it.
        /// </summary>
        public static T Run<T>(IWin32Window owner, string title, Func<Progress, CancellationToken, T> work)
        {
            var progress = new Progress();
            using (var cancel = new CancellationTokenSource())
            {
                Task<T> task = Task.Run(() => work(progress, cancel.Token));
                // ---- short work: no window ------------------------------------------------------
                bool done = false;
                try
                {
                    done = task.Wait(350);
                }
                catch (AggregateException)
                {
                    done = true;
                }
                if (!done)
                {
                    using (var dlg = new ProgressDialog(title, progress, cancel))
                    {
                        dlg.CentreOn(owner);
                        task.ContinueWith(_ => dlg.FinishFromWorker(), TaskScheduler.Default);
                        dlg.ShowDialog(owner);
                    }
                }
                try
                {
                    return task.GetAwaiter().GetResult();
                }
                catch (Exception ex)
                {
                    Log.Error("background work '" + title + "' failed", ex);
                    throw;
                }
            }
        }

        /// <summary>Called on the worker's thread when it ends: close on the UI thread.</summary>
        private void FinishFromWorker()
        {
            try
            {
                if (IsHandleCreated && !IsDisposed)
                {
                    BeginInvoke(new Action(() =>
                    {
                        _finished = true;
                        Close();
                    }));
                }
                else
                {
                    _finished = true;
                }
            }
            catch (Exception)
            {
                // The window is already gone.
            }
        }

        protected override void OnShown(EventArgs e)
        {
            base.OnShown(e);
            // The worker may have ended between Run's wait and the window
            // appearing: close at once instead of waiting for a callback that
            // already happened.
            if (_finished)
            {
                Close();
                return;
            }
            _poll.Start();
        }

        protected override void OnFormClosing(FormClosingEventArgs e)
        {
            // Only the worker's end closes the sheet; Cancel asks the worker to stop.
            if (!_finished)
            {
                e.Cancel = true;
                RequestCancel();
                return;
            }
            _poll.Stop();
            base.OnFormClosing(e);
        }

        protected override void OnEscape() => RequestCancel();

        private void RequestCancel()
        {
            try
            {
                if (!_cancel.IsCancellationRequested)
                {
                    _cancel.Cancel();
                    _cancelButton.Text = "Stopping...";
                    _cancelButton.Enabled = false;
                    _line = "Stopping after the current step...";
                    Invalidate();
                }
            }
            catch (Exception)
            {
                // Disposed: the work is over.
            }
        }

        private void Poll()
        {
            _progress.Read(out double fraction, out string text);
            if (!_cancel.IsCancellationRequested && text != _line)
            {
                _line = text;
                Invalidate();
            }
            if (Math.Abs(_bar.Target - fraction) > 1e-4)
            {
                Animator.To(this, _bar, fraction);
            }
        }

        protected override void Dispose(bool disposing)
        {
            if (disposing)
            {
                _poll.Dispose();
            }
            base.Dispose(disposing);
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            try
            {
                Graphics g = e.Graphics;
                Draw.Prepare(g);
                Theme t = Theme.Current;
                float s = S;
                Draw.Text(g, _title, TextStyle.BodyStrong, new Rectangle(Px(16), Px(14), Width - Px(32), Px(22)), t.Text);
                Draw.Text(g, _line, TextStyle.Caption, new Rectangle(Px(16), Px(38), Width - Px(32), Px(18)), t.SecondaryText);
                var track = new RectangleF(Px(16), Px(64), Width - Px(32), 6f * s);
                Draw.Fill(g, track, 3f * s, t.ControlFill);
                float w = (float)(track.Width * Math.Max(0, Math.Min(1, _bar.Value)));
                if (w > 1f)
                {
                    Draw.Fill(g, new RectangleF(track.X, track.Y, Math.Max(6f * s, w), track.Height), 3f * s, t.Accent);
                }
                string pct = ((int)Math.Round(_bar.Value * 100)).ToString(System.Globalization.CultureInfo.CurrentCulture) + "%";
                Draw.Text(g, pct, TextStyle.Micro, new Rectangle(Px(16), Px(74), Px(60), Px(16)), t.TertiaryText, TextFormatFlags.Left | TextFormatFlags.Top);
            }
            catch (Exception ex)
            {
                Log.Warn("progress: painting failed", ex);
            }
        }
    }
}
