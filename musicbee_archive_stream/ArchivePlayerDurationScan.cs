// Archive Stream Player / MusicBee: playback-triggered duration verification.
// Initially list ID3 metadata without opening/decoding the archived audio.
// Once the first track in an archive starts, verify PCM lengths for that
// archive on a CPU+I/O background-priority thread, prioritising the playing
// track. Reuse the existing per-entry cache, and never calculate CRCs.
// MusicBee owns its native Duration column; SDK gives us no setter for it.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        // One background metadata decoder at a time, even if the listener
        // quickly skips between archives. Playback retains I/O priority.
        private static readonly SemaphoreSlim archiveDurationWorkGate = new SemaphoreSlim(1, 1);
        private readonly object archiveDurationScanLock = new object();
        private readonly HashSet<string> archiveDurationScansActive =
            new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        private readonly HashSet<string> archiveDurationScansCompleted =
            new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        private const int ThreadModeBackgroundBegin = 0x00010000;
        private const int ThreadModeBackgroundEnd = 0x00020000;

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr GetCurrentThread();

        [DllImport("kernel32.dll", SetLastError = true)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool SetThreadPriority(IntPtr thread, int priority);

        private void ScheduleArchiveDurationScanOnPlayback(string source)
        {
            if (archiveDropClosing || !nativeMp3Loaded ||
                !IsPersistentNativeLibraryTrack(source))
                return;

            Control root = archiveDropRoot;
            if (root == null || root.IsDisposed || !root.IsHandleCreated)
                return;

            if (root.InvokeRequired)
            {
                try
                {
                    root.BeginInvoke((Action)(() => ScheduleArchiveDurationScanOnPlayback(source)));
                }
                catch (InvalidOperationException) { }
                return;
            }

            if (api.Player_GetPlayState == null ||
                api.Player_GetPlayState() != PlayState.Playing ||
                api.Playlist_QueryFilesEx == null || api.Playlist_SetFiles == null)
                return;

            string archive, member, virtualPath;
            if (!TryReadArchiveEntryPath(source, out archive, out member, out virtualPath))
                return;

            string playlist = FindArchivePlayerPlaylist();
            if (String.IsNullOrWhiteSpace(playlist))
                return;

            string[] snapshot;
            if (!api.Playlist_QueryFilesEx(playlist, out snapshot) || snapshot == null ||
                snapshot.Length == 0 || snapshot.Length > 10000)
                return;

            // No descriptor enumeration on MusicBee's UI thread. It is
            // enough here to confirm that the current alias belongs to this
            // playlist; the worker discovers its sibling tracks later.
            if (!snapshot.Contains(source, StringComparer.OrdinalIgnoreCase))
                return;

            // The same pathname can be replaced during a MusicBee session.
            // Complete status is tied to this archive version, not merely its
            // filename. Per-entry caches have the same size+mtime guard.
            string scanKey;
            try
            {
                var file = new FileInfo(archive);
                if (!file.Exists)
                    return;
                scanKey = archive + "|" + file.Length + "|" + file.LastWriteTimeUtc.Ticks;
            }
            catch (IOException) { return; }
            catch (UnauthorizedAccessException) { return; }

            lock (archiveDurationScanLock)
            {
                if (archiveDurationScansActive.Contains(scanKey) ||
                    archiveDurationScansCompleted.Contains(scanKey))
                    return;
                archiveDurationScansActive.Add(scanKey);
            }

            bool useTags = UseArchiveFileTags();
            string settingSnapshot = CurrentArchiveMetadataSettings();
            Trace("ARCHIVE_DURATION_SCAN_QUEUED playlist_tracks=" + snapshot.Length +
                " trigger=playback scope=current_archive priority=background");

            var worker = new Thread(() =>
            {
                bool submitted = false;
                int verified = 0;
                int failed = 0;
                int[] indices = new int[0];
                string[] updated = (string[])snapshot.Clone();
                try
                {
                    // Winamp uses THREAD_MODE_BACKGROUND_BEGIN for low CPU and
                    // low disk-I/O priority. Use the same Windows thread mode,
                    // as well as a managed BelowNormal fallback priority.
                    bool background = SetThreadPriority(GetCurrentThread(), ThreadModeBackgroundBegin);
                    archiveDurationWorkGate.Wait();
                    try
                    {
                        // Identify archive siblings after lowering both CPU
                        // and I/O scheduling priority, not on the host UI.
                        indices = ArchiveDurationScanIndices(snapshot, source);
                        if (indices.Length == 0 || indices.Length > 2000)
                        {
                            Trace("ARCHIVE_DURATION_SCAN_SKIPPED reason=no_tracks_or_entry_cap count=" +
                                indices.Length);
                            return;
                        }
                        foreach (int i in indices)
                        {
                            if (archiveDropClosing || root.IsDisposed)
                                break;
                            try
                            {
                                if (!IsPersistentNativeLibraryTrack(snapshot[i]))
                                {
                                    ++failed;
                                    continue;
                                }
                                string descriptor = Path.ChangeExtension(snapshot[i], ".ast");
                                // Opens the paired manifest with the native BASS
                                // decoder. BASS verifies frame count/PCM length.
                                // Cached durations are reused without decoding.
                                long durationMs = QueryArchiveDurationMs(snapshot[i]);
                                if (durationMs == 0)
                                {
                                    ++failed;
                                    continue;
                                }
                                string replacement = VersionedMusicBeeAlias(
                                    descriptor, useTags, snapshot[i], true);
                                if (!AliasHasMetadataMarker(replacement, useTags, true))
                                {
                                    ++failed;
                                    continue;
                                }
                                updated[i] = replacement;
                                ++verified;
                            }
                            catch (Exception error)
                            {
                                ++failed;
                                Trace("ARCHIVE_DURATION_ENTRY_FAIL type=" + error.GetType().Name);
                            }
                        }
                    }
                    finally
                    {
                        archiveDurationWorkGate.Release();
                        if (background)
                            SetThreadPriority(GetCurrentThread(), ThreadModeBackgroundEnd);
                    }

                    if (archiveDropClosing || root.IsDisposed || !root.IsHandleCreated)
                        return;

                    root.BeginInvoke((Action)(() =>
                    {
                        bool committed = false;
                        try
                        {
                            if (archiveDropClosing || api.Playlist_QueryFilesEx == null ||
                                api.Playlist_SetFiles == null)
                                return;

                            string[] current;
                            if (!api.Playlist_QueryFilesEx(playlist, out current) || current == null ||
                                !current.SequenceEqual(snapshot, StringComparer.OrdinalIgnoreCase))
                            {
                                Trace("ARCHIVE_DURATION_SCAN_APPLY_SKIPPED reason=playlist_changed");
                                return;
                            }
                            if (!String.Equals(CurrentArchiveMetadataSettings(),
                                settingSnapshot, StringComparison.Ordinal))
                            {
                                Trace("ARCHIVE_DURATION_SCAN_APPLY_SKIPPED reason=tag_settings_changed");
                                return;
                            }

                            bool changed = !updated.SequenceEqual(snapshot,
                                StringComparer.OrdinalIgnoreCase);
                            string beforeState = api.Player_GetPlayState == null ? "unknown" :
                                api.Player_GetPlayState().ToString();
                            committed = !changed || api.Playlist_SetFiles(playlist, updated);
                            string afterState = api.Player_GetPlayState == null ? "unknown" :
                                api.Player_GetPlayState().ToString();
                            Trace("ARCHIVE_DURATION_SCAN_DONE verified=" + verified +
                                " failed=" + failed + " changed=" + (changed ? "1" : "0") +
                                " commit=" + (committed ? "1" : "0") +
                                " playback_before=" + beforeState +
                                " playback_after=" + afterState);
                            if (committed && api.MB_RefreshPanels != null)
                                api.MB_RefreshPanels();
                        }
                        catch (Exception error)
                        {
                            Trace("ARCHIVE_DURATION_SCAN_APPLY_FAIL type=" + error.GetType().Name);
                        }
                        finally
                        {
                            FinishArchiveDurationScan(scanKey, committed && failed == 0 &&
                                verified == indices.Length);
                        }
                    }));
                    submitted = true;
                }
                catch (Exception error)
                {
                    Trace("ARCHIVE_DURATION_SCAN_FAIL type=" + error.GetType().Name);
                }
                finally
                {
                    // If the host closed before the queued UI action could be
                    // submitted, release the in-flight flag for future retry.
                    if (!submitted)
                        FinishArchiveDurationScan(scanKey, false);
                }
            });
            worker.Name = "Archive Stream MusicBee duration verification";
            worker.IsBackground = true;
            worker.Priority = ThreadPriority.BelowNormal;
            worker.Start();
        }

        private void FinishArchiveDurationScan(string archive, bool complete)
        {
            lock (archiveDurationScanLock)
            {
                archiveDurationScansActive.Remove(archive);
                if (complete)
                    archiveDurationScansCompleted.Add(archive);
            }
        }
    }
}
