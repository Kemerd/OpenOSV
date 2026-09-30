// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// OsvMedia.cs - one OpenOSV Source generated media in the project, and the
// events that play it.

using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using OpenOSV.Vegas.Core;
using ScriptPortal.Vegas;

namespace OpenOSV.Vegas.Host
{
    /// <summary>
    /// A generated media whose generator is OpenOSV Source.
    /// </summary>
    /// <remarks>
    /// <para>
    /// VEGAS keeps a generator's parameters on the MEDIA, not on the event:
    /// every event cut from one OpenOSV media shares its file, its colour and
    /// its camera.  That is why the panel edits clips (media) and why "Make
    /// framing unique" exists.
    /// </para>
    /// <para>
    /// Identity is the generator itself (its plug-in's unique id), never the
    /// comment or the custom data, so a media made by hand from the Media
    /// Generators window counts too.
    /// </para>
    /// </remarks>
    internal sealed class OsvMedia
    {
        private OsvMedia(Media media, OFXEffect fx)
        {
            Media = media;
            Fx = fx;
        }

        /// <summary>The VEGAS media.</summary>
        public Media Media { get; }

        /// <summary>Its generator's OpenFX parameters.</summary>
        public OFXEffect Fx { get; }

        /// <summary>Wrap a media when it is OpenOSV Source's, else null.  Never throws.</summary>
        public static OsvMedia TryWrap(Media media)
        {
            if (media is null)
            {
                return null;
            }
            try
            {
                if (!media.IsValid() || !media.IsGenerated())
                {
                    return null;
                }
                Effect generator = media.Generator;
                if (!VegasHost.IsPlugIn(generator, OfxIds.SourcePluginId))
                {
                    return null;
                }
                OFXEffect fx = Ofx.Of(generator);
                return fx is null ? null : new OsvMedia(media, fx);
            }
            catch (Exception ex)
            {
                Log.Debug("media: not an OpenOSV media (" + ex.Message + ")");
                return null;
            }
        }

        /// <summary>Every OpenOSV media in the project's media pool.</summary>
        public static List<OsvMedia> AllIn(Project project)
        {
            var list = new List<OsvMedia>();
            if (project is null)
            {
                return list;
            }
            try
            {
                foreach (object o in project.MediaPool.Values)
                {
                    OsvMedia m = TryWrap(o as Media);
                    if (m is not null)
                    {
                        list.Add(m);
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Warn("media: listing the media pool failed", ex);
            }
            return list;
        }

        /// <summary>The OpenOSV media an event plays (its active take's), or null.</summary>
        public static OsvMedia ForEvent(TrackEvent evt)
        {
            try
            {
                return TryWrap(evt?.ActiveTake?.Media);
            }
            catch (Exception)
            {
                return null;
            }
        }

        // =====================================================================
        //  State
        // =====================================================================

        /// <summary>A stable key for this media within a session (the pool key).</summary>
        public string Key => VegasHost.SafeString(() => Media.KeyString) ?? string.Empty;

        /// <summary>The clip the generator plays (the <c>file</c> parameter).</summary>
        public string FilePath => (Ofx.GetString(Fx, SourceParams.File) ?? string.Empty).Trim().Trim('"');

        /// <summary>The full-quality clip: the record's source, else the file, else the comment.</summary>
        public string SourcePath
        {
            get
            {
                MediaRecord r = Record;
                if (r is not null && !string.IsNullOrEmpty(r.Source))
                {
                    return r.Source;
                }
                string file = FilePath;
                if (SiblingFiles.IsLrf(file))
                {
                    string original = SiblingFiles.OriginalOf(file);
                    if (original is not null)
                    {
                        return original;
                    }
                }
                if (!string.IsNullOrEmpty(file))
                {
                    return file;
                }
                return VegasHost.SafeString(() => Media.Comment) ?? string.Empty;
            }
        }

        /// <summary>A display name: the clip's file name.</summary>
        public string DisplayName
        {
            get
            {
                string path = SourcePath;
                string name = string.IsNullOrEmpty(path) ? string.Empty : ProbeResultName(path);
                return string.IsNullOrEmpty(name) ? OfxIds.SourceLabel : name;
            }
        }

        /// <summary>True while the generator plays an .LRF.</summary>
        public bool OnProxy => SiblingFiles.IsLrf(FilePath);

        /// <summary>True when the clip the generator plays is missing on disk.</summary>
        public bool IsOffline
        {
            get
            {
                string f = FilePath;
                return string.IsNullOrEmpty(f) || !SiblingFiles.Exists(f);
            }
        }

        /// <summary>The Output entry (0 = Reframed view, 1 = 360 equirect).</summary>
        public int Output => Ofx.GetChoice(Fx, SourceParams.Output, Choices.OutputReframed);

        /// <summary>The Colour Output entry.</summary>
        public int ColorOutput => Ofx.GetChoice(Fx, StitchParams.ColorOutput, Choices.ColorOutput.Default0);

        /// <summary>The Output Levels entry, or -1 when the plug-in has no such control.</summary>
        public int OutputLevels => Ofx.Has(Fx, SourceParams.OutputLevels) ? Ofx.GetChoice(Fx, SourceParams.OutputLevels, Choices.LevelsStudio) : -1;

        /// <summary>The Stabilisation entry.</summary>
        public int Stabilization => Ofx.GetChoice(Fx, StitchParams.Stabilization, Choices.Stabilization.Default0);

        /// <summary>The Start Frame.</summary>
        public int StartFrame => Ofx.GetInt(Fx, SourceParams.StartFrame, 0);

        /// <summary>The generated frame size (empty when unknown).</summary>
        public Size FrameSize
        {
            get
            {
                try
                {
                    VideoStream vs = Media.GetVideoStreamByIndex(0);
                    return vs is not null ? vs.Size : Size.Empty;
                }
                catch (Exception)
                {
                    return Size.Empty;
                }
            }
        }

        /// <summary>The media's length.</summary>
        public Timecode Length
        {
            get
            {
                try
                {
                    return Media.Length;
                }
                catch (Exception)
                {
                    return null;
                }
            }
        }

        /// <summary>The generated video stream, or null.</summary>
        public VideoStream VideoStream
        {
            get
            {
                try
                {
                    return Media.GetVideoStreamByIndex(0);
                }
                catch (Exception)
                {
                    return null;
                }
            }
        }

        // =====================================================================
        //  The extension's own record (custom data)
        // =====================================================================

        /// <summary>The record stored on the media, or null.</summary>
        public MediaRecord Record
        {
            get
            {
                try
                {
                    byte[] bytes = Media.CustomData?.GetBytes(MediaRecord.DataId);
                    return MediaRecord.FromBytes(bytes);
                }
                catch (Exception)
                {
                    return null;
                }
            }
        }

        /// <summary>Store the record (and mirror its source into the media comment).</summary>
        public bool SaveRecord(MediaRecord record)
        {
            if (record is null)
            {
                return false;
            }
            bool ok = true;
            try
            {
                Media.CustomData.SetBytes(MediaRecord.DataId, record.ToBytes());
            }
            catch (Exception ex)
            {
                ok = false;
                Log.Warn("media: storing the OpenOSV record failed (the extension re-derives it when missing)", ex);
            }
            SetComment(record.Source);
            return ok;
        }

        /// <summary>The record, or a new one derived from the parameters.</summary>
        public MediaRecord RecordOrDerived()
        {
            MediaRecord r = Record;
            if (r is not null)
            {
                return r;
            }
            string file = FilePath;
            r = new MediaRecord();
            if (SiblingFiles.IsLrf(file))
            {
                string original = SiblingFiles.OriginalOf(file);
                r.Source = original ?? file;
                r.Proxy = original is not null ? file : string.Empty;
                r.OnProxy = original is not null;
            }
            else
            {
                r.Source = file;
                r.Proxy = SiblingFiles.ProxyOf(file) ?? string.Empty;
                r.FullStartFrame = StartFrame;
            }
            return r;
        }

        /// <summary>Set the media comment (the path people see in Project Media).</summary>
        public void SetComment(string text)
        {
            try
            {
                Media.Comment = text ?? string.Empty;
            }
            catch (Exception ex)
            {
                Log.Debug("media: setting the comment failed: " + ex.Message);
            }
        }

        // =====================================================================
        //  Events
        // =====================================================================

        /// <summary>Every video event in the project whose active take plays this media.</summary>
        public List<VideoEvent> Events(Project project)
        {
            var list = new List<VideoEvent>();
            if (project is null)
            {
                return list;
            }
            try
            {
                foreach (Track track in project.Tracks)
                {
                    if (!(track is VideoTrack))
                    {
                        continue;
                    }
                    foreach (TrackEvent evt in track.Events)
                    {
                        if (evt is VideoEvent ve && SameMedia(ve.ActiveTake?.Media, Media))
                        {
                            list.Add(ve);
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Log.Warn("media: walking the timeline failed", ex);
            }
            return list;
        }

        /// <summary>True when two media are the same pool entry.</summary>
        public static bool SameMedia(Media a, Media b)
        {
            if (a is null || b is null)
            {
                return false;
            }
            try
            {
                return a.Equals(b) || string.Equals(a.KeyString, b.KeyString, StringComparison.Ordinal);
            }
            catch (Exception)
            {
                return ReferenceEquals(a, b);
            }
        }

        private static string ProbeResultName(string path)
        {
            try
            {
                return Path.GetFileName(path);
            }
            catch (Exception)
            {
                return path;
            }
        }
    }
}
