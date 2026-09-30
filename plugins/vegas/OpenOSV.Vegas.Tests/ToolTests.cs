// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// ToolTests.cs - the process runner against Windows' own cmd.exe, and osvtool
// against the sample clip when ctest hands both over (CPU only: probe and
// audio extraction decode no video).

using System;
using System.Diagnostics;
using System.IO;
using System.Threading;
using OpenOSV.Vegas.Core;

namespace OpenOSV.Vegas.Tests
{
    public static class ProcessRunnerTests
    {
        private static string Cmd
        {
            get
            {
                string system = Environment.GetFolderPath(Environment.SpecialFolder.System);
                return Path.Combine(system, "cmd.exe");
            }
        }

        [Test]
        public static void CapturesOutputErrorsAndExitCodes()
        {
            ProcessOutcome run = ProcessRunner.Run(Cmd, new[] { "/c", "echo out& echo error: it broke 1>&2& exit 3" }, null,
                                                   TimeSpan.FromSeconds(20), CancellationToken.None);
            Check.Null(run.LaunchError, "launched");
            Check.Equal(3, run.ExitCode, "exit code");
            Check.Contains(run.StdOut, "out", "stdout");
            Check.Contains(run.StdErr, "it broke", "stderr");
            Check.Equal("it broke", run.Headline, "headline from the error line");
            Check.False(run.Succeeded, "non-zero exit is failure");
        }

        [Test]
        public static void TimeoutKillsTheTool()
        {
            var watch = Stopwatch.StartNew();
            ProcessOutcome run = ProcessRunner.Run(Cmd, new[] { "/c", "ping -n 30 127.0.0.1 >nul" }, null,
                                                   TimeSpan.FromMilliseconds(300), CancellationToken.None);
            Check.True(run.TimedOut, "timed out");
            Check.True(watch.Elapsed < TimeSpan.FromSeconds(15), "returned promptly");
        }

        [Test]
        public static void CancellationKillsTheTool()
        {
            using (var cts = new CancellationTokenSource(300))
            {
                ProcessOutcome run = ProcessRunner.Run(Cmd, new[] { "/c", "ping -n 30 127.0.0.1 >nul" }, null,
                                                       TimeSpan.FromMinutes(1), cts.Token);
                Check.True(run.Cancelled, "cancelled");
                Check.Equal("cancelled", run.Headline, "headline");
            }
        }

        [Test]
        public static void AMissingToolIsALaunchErrorNotAnException()
        {
            ProcessOutcome run = ProcessRunner.Run(@"Z:\nowhere\osvtool.exe", new[] { "probe" }, null, TimeSpan.FromSeconds(5),
                                                   CancellationToken.None);
            Check.NotNull(run.LaunchError, "launch error");
            Check.False(run.Succeeded, "not a success");
        }
    }

    public static class OsvToolTests
    {
        private static OsvTool Tool()
        {
            if (string.IsNullOrEmpty(TestContext.OsvTool) || !File.Exists(TestContext.OsvTool))
            {
                throw new SkipException("no --osvtool");
            }
            if (string.IsNullOrEmpty(TestContext.SampleOsv) || !File.Exists(TestContext.SampleOsv))
            {
                throw new SkipException("no sample clip (--sample or OSV_SAMPLE_FILE)");
            }
            return new OsvTool(TestContext.OsvTool);
        }

        [Test]
        public static void LocatePrefersTheUsersChoice()
        {
            if (string.IsNullOrEmpty(TestContext.OsvTool) || !File.Exists(TestContext.OsvTool))
            {
                throw new SkipException("no --osvtool");
            }
            OsvToolLocation loc = OsvTool.Locate(TestContext.OsvTool);
            Check.True(loc.Found, "found");
            Check.Equal(OsvToolSource.UserSetting, loc.Source, "the user's choice wins");
            OsvToolLocation bogus = OsvTool.Locate(@"Z:\nowhere\osvtool.exe");
            Check.True(bogus.Searched.Count >= 2, "a missing choice falls through to the other places");
        }

        [Test]
        public static void ProbesTheSampleClip()
        {
            OsvTool tool = Tool();
            ProbeResult p = tool.Probe(TestContext.SampleOsv, CancellationToken.None, out string error);
            Check.NotNull(p, "probe: " + error);
            Check.True(p.IsUsable, "usable");
            Check.True(p.Fps.IsValid, "rational rate");
            Check.True(p.FrameCount > 0, "frames");
            Check.True(p.SphereWidth == 2 * p.SphereHeight, "2:1 sphere");
            // Whatever build of osvtool this is, no stray "-" file is left behind.
            Check.False(File.Exists(Path.Combine(Environment.CurrentDirectory, "-")), "no stray '-' file");
        }

        [Test]
        public static void ProbingANonClipFailsCleanly()
        {
            OsvTool tool = Tool();
            string notAClip = Path.Combine(TestContext.NewDir("notclip"), "fake.OSV");
            File.WriteAllText(notAClip, "this is not an mp4 container");
            ProbeResult p = tool.Probe(notAClip, CancellationToken.None, out string error);
            Check.Null(p, "no result");
            Check.NotNull(error, "an error line");
        }

        /// <summary>
        /// The audio path end to end: a WAV when this osvtool writes WAV, else
        /// the AAC fallback - with the ADTS-in-a-.wav case detected, never
        /// handed to VEGAS as a WAV.
        /// </summary>
        [Test]
        public static void ExtractsAudioIntoTheCache()
        {
            OsvTool tool = Tool();
            var cache = new AudioCache(TestContext.NewDir("audio"));
            AudioExtraction wav = tool.ExtractWav(TestContext.SampleOsv, cache, 2.0, CancellationToken.None);
            if (wav.Succeeded)
            {
                Check.True(wav.IsWav, "a WAV");
                Check.True(wav.Wav.IsFloat32, "32-bit float");
                Check.True(wav.Wav.DurationSeconds > 0.5, "has length");
                // Sample-exact: as many frames as the probe says the track holds.
                ProbeResult probe = tool.Probe(TestContext.SampleOsv, CancellationToken.None, out _);
                if (probe != null && probe.AudioSampleCount > 0)
                {
                    Check.Equal(probe.AudioSampleCount, wav.Wav.Frames, "WAV frames = the probe's audio sample count");
                    Check.Equal(probe.AudioChannels, wav.Wav.Channels, "channels");
                    Check.Equal(probe.AudioSampleRate, wav.Wav.SampleRate, "sample rate");
                }
                AudioExtraction again = tool.ExtractWav(TestContext.SampleOsv, cache, 2.0, CancellationToken.None);
                Check.True(again.FromCache, "reused the second time");
                Console.WriteLine("        note: WAV path (" + wav.Wav.Frames + " frames, " + wav.Wav.Channels + " ch, " + wav.Wav.SampleRate + " Hz)");
                return;
            }
            Check.True(wav.WavUnsupported, "an osvtool without WAV support is recognised (" + wav.Error + ")");
            Check.False(File.Exists(cache.PathFor(TestContext.SampleOsv, ".wav")), "no AAC left behind as .wav");
            AudioExtraction aac = tool.ExtractAac(TestContext.SampleOsv, cache, 2.0, CancellationToken.None);
            Check.True(aac.Succeeded, "AAC fallback: " + aac.Error);
            Check.Equal(AudioFileKind.AdtsAac, WavFile.Sniff(aac.Path), "ADTS");
            Console.WriteLine("        note: this osvtool writes AAC only; the WAV path waits for WP-V-CLI");
        }
    }
}
