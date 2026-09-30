// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// WavFile.cs - read just enough of a WAV header to trust the file.

using System;
using System.IO;
using System.Text;

namespace OpenOSV.Vegas.Core
{
    /// <summary>What the first bytes of an audio file say it is.</summary>
    public enum AudioFileKind
    {
        /// <summary>Missing, empty or unrecognised.</summary>
        Unknown,
        /// <summary>RIFF / RF64 WAVE.</summary>
        Wav,
        /// <summary>A raw AAC ADTS stream (what older osvtool builds write for any extension).</summary>
        AdtsAac,
    }

    /// <summary>The facts of a WAV file's header.</summary>
    public sealed class WavInfo
    {
        /// <summary>1 = PCM, 3 = IEEE float, 0xFFFE = extensible.</summary>
        public int FormatTag { get; internal set; }

        /// <summary>The sample format after resolving WAVE_FORMAT_EXTENSIBLE (1 or 3).</summary>
        public int SampleFormat { get; internal set; }

        /// <summary>Channel count.</summary>
        public int Channels { get; internal set; }

        /// <summary>Samples per second.</summary>
        public int SampleRate { get; internal set; }

        /// <summary>Bits per sample.</summary>
        public int BitsPerSample { get; internal set; }

        /// <summary>Bytes per sample frame (all channels).</summary>
        public int BlockAlign { get; internal set; }

        /// <summary>Size of the audio data in bytes.</summary>
        public long DataBytes { get; internal set; }

        /// <summary>Number of sample frames.</summary>
        public long Frames => BlockAlign > 0 ? DataBytes / BlockAlign : 0;

        /// <summary>Duration in seconds.</summary>
        public double DurationSeconds => SampleRate > 0 ? (double)Frames / SampleRate : 0.0;

        /// <summary>True for 32-bit IEEE float samples (what osvtool writes).</summary>
        public bool IsFloat32 => SampleFormat == 3 && BitsPerSample == 32;
    }

    /// <summary>WAV header reading and audio file sniffing.</summary>
    public static class WavFile
    {
        /// <summary>
        /// Sniff the first bytes of a file.  Never throws; a missing or
        /// unreadable file is <see cref="AudioFileKind.Unknown"/>.
        /// </summary>
        public static AudioFileKind Sniff(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                {
                    var head = new byte[12];
                    int n = fs.Read(head, 0, head.Length);
                    if (n >= 12)
                    {
                        string riff = Encoding.ASCII.GetString(head, 0, 4);
                        string wave = Encoding.ASCII.GetString(head, 8, 4);
                        if ((riff == "RIFF" || riff == "RF64") && wave == "WAVE")
                        {
                            return AudioFileKind.Wav;
                        }
                    }
                    // ADTS: 12-bit sync word 0xFFF, layer bits 00.
                    if (n >= 2 && head[0] == 0xFF && (head[1] & 0xF6) == 0xF0)
                    {
                        return AudioFileKind.AdtsAac;
                    }
                }
            }
            catch (Exception)
            {
                // Unreadable: unknown.
            }
            return AudioFileKind.Unknown;
        }

        /// <summary>
        /// Read the header of a RIFF / RF64 WAVE file.  Returns null (never
        /// throws) for anything that is not a playable PCM or float WAV with a
        /// non-empty data chunk.
        /// </summary>
        public static WavInfo TryRead(string path)
        {
            try
            {
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete))
                using (var br = new BinaryReader(fs, Encoding.ASCII))
                {
                    long fileLength = fs.Length;
                    if (fileLength < 44)
                    {
                        return null;
                    }
                    string riff = new string(br.ReadChars(4));
                    br.ReadUInt32();
                    string wave = new string(br.ReadChars(4));
                    if ((riff != "RIFF" && riff != "RF64") || wave != "WAVE")
                    {
                        return null;
                    }

                    // ---- walk the chunks ------------------------------------------------
                    var info = new WavInfo();
                    long ds64Data = -1;
                    bool haveFmt = false;
                    int guard = 0;
                    while (fs.Position + 8 <= fileLength && guard++ < 4096)
                    {
                        string id = new string(br.ReadChars(4));
                        long size = br.ReadUInt32();
                        long bodyStart = fs.Position;
                        if (id == "ds64" && size >= 16)
                        {
                            br.ReadUInt64(); // RIFF size
                            ds64Data = (long)br.ReadUInt64();
                        }
                        else if (id == "fmt " && size >= 16)
                        {
                            info.FormatTag = br.ReadUInt16();
                            info.Channels = br.ReadUInt16();
                            info.SampleRate = (int)br.ReadUInt32();
                            br.ReadUInt32(); // byte rate
                            info.BlockAlign = br.ReadUInt16();
                            info.BitsPerSample = br.ReadUInt16();
                            info.SampleFormat = info.FormatTag;
                            if (info.FormatTag == 0xFFFE && size >= 40)
                            {
                                br.ReadUInt16(); // cbSize
                                br.ReadUInt16(); // valid bits
                                br.ReadUInt32(); // channel mask
                                // The sub-format GUID's first two bytes are the format tag.
                                info.SampleFormat = br.ReadUInt16();
                            }
                            haveFmt = true;
                        }
                        else if (id == "data")
                        {
                            long remaining = fileLength - bodyStart;
                            long dataSize = size;
                            if (riff == "RF64" && size == 0xFFFFFFFFL && ds64Data >= 0)
                            {
                                dataSize = ds64Data;
                            }
                            // A writer that could not finish its header leaves
                            // 0 or a size past the end: trust the file length.
                            if (dataSize <= 0 || dataSize > remaining)
                            {
                                dataSize = remaining;
                            }
                            info.DataBytes = dataSize;
                            break;
                        }
                        // Chunks are word-aligned.
                        long next = bodyStart + size + (size & 1);
                        if (next <= bodyStart || next > fileLength)
                        {
                            break;
                        }
                        fs.Position = next;
                    }

                    // ---- is it usable? --------------------------------------------------------
                    bool knownFormat = info.SampleFormat == 1 || info.SampleFormat == 3;
                    if (!haveFmt || !knownFormat || info.Channels <= 0 || info.Channels > 64 || info.SampleRate < 1000 ||
                        info.SampleRate > 768000 || info.BlockAlign <= 0 || info.DataBytes <= 0)
                    {
                        return null;
                    }
                    return info;
                }
            }
            catch (Exception)
            {
                return null;
            }
        }
    }
}
