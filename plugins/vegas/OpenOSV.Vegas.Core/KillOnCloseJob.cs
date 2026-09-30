// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 The OpenOSV Contributors
//
// KillOnCloseJob.cs - a Windows job object that ends a tool's whole process
// tree when the run is over, cancelled, or VEGAS itself exits.

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace OpenOSV.Vegas.Core
{
    /// <summary>
    /// One Windows job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, holding
    /// one tool process.
    /// </summary>
    /// <remarks>
    /// <para>
    /// <see cref="Process.Kill()"/> ends one process; a job ends the tree.  And
    /// because the job dies with its last handle, an osvtool extracting an
    /// hour of audio is stopped when VEGAS closes (or crashes) instead of
    /// running on as an orphan that holds the cache file open.
    /// </para>
    /// <para>
    /// Everything is best effort: if Windows refuses any step (an old system
    /// without nested jobs, a process already in a job that forbids breakaway)
    /// the runner falls back to killing the process alone.
    /// </para>
    /// </remarks>
    internal sealed class KillOnCloseJob : IDisposable
    {
        private IntPtr _handle;

        private KillOnCloseJob(IntPtr handle)
        {
            _handle = handle;
        }

        /// <summary>A job holding <paramref name="process"/>, or null when Windows declines.</summary>
        public static KillOnCloseJob TryCreateFor(Process process)
        {
            if (process == null)
            {
                return null;
            }
            IntPtr job = IntPtr.Zero;
            try
            {
                job = CreateJobObjectW(IntPtr.Zero, null);
                if (job == IntPtr.Zero)
                {
                    return null;
                }
                var info = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION();
                info.BasicLimitInformation.LimitFlags = JobObjectLimitKillOnJobClose;
                int size = Marshal.SizeOf(typeof(JOBOBJECT_EXTENDED_LIMIT_INFORMATION));
                IntPtr buffer = Marshal.AllocHGlobal(size);
                try
                {
                    Marshal.StructureToPtr(info, buffer, false);
                    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, buffer, (uint)size))
                    {
                        CloseHandle(job);
                        return null;
                    }
                }
                finally
                {
                    Marshal.FreeHGlobal(buffer);
                }
                if (!AssignProcessToJobObject(job, process.Handle))
                {
                    CloseHandle(job);
                    return null;
                }
                return new KillOnCloseJob(job);
            }
            catch (Exception)
            {
                if (job != IntPtr.Zero)
                {
                    CloseHandle(job);
                }
                return null;
            }
        }

        /// <summary>End every process in the job now.</summary>
        public void Terminate()
        {
            try
            {
                if (_handle != IntPtr.Zero)
                {
                    TerminateJobObject(_handle, 1);
                }
            }
            catch (Exception)
            {
                // The caller kills the process itself as well.
            }
        }

        /// <summary>Close the job (which kills whatever is still in it).</summary>
        public void Dispose()
        {
            IntPtr h = _handle;
            _handle = IntPtr.Zero;
            if (h != IntPtr.Zero)
            {
                try
                {
                    CloseHandle(h);
                }
                catch (Exception)
                {
                    // Nothing more to do.
                }
            }
        }

        // ---- Win32 ------------------------------------------------------------------

        private const uint JobObjectLimitKillOnJobClose = 0x2000;
        private const int JobObjectExtendedLimitInformation = 9;

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IO_COUNTERS
        {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr CreateJobObjectW(IntPtr attributes, string name);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetInformationJobObject(IntPtr job, int infoClass, IntPtr info, uint length);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool TerminateJobObject(IntPtr job, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool CloseHandle(IntPtr handle);
    }
}
