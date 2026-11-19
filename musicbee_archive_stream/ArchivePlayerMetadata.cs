// Embedded archive-entry tags are read off the MusicBee UI thread from a
// bounded prefix of independently streamable entries. No full extraction,
// no content CRC calculation, and no writes into archive/descriptor bytes.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;
using Microsoft.Win32;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamReadNativeFileTagsW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int ReadNativeArchiveFileTags(string virtualPath, StringBuilder output, uint capacity);

        private sealed class ArchiveEntryMetadata
        {
            public string path;
            public string title;
            public bool hasEmbeddedTitle;
            public string artist;
            public string album;
            public string albumArtist;
            public string genre;
            public string year;
            public string number;
        }

        private static bool UseArchiveFileTags()
        {
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
            {
                return key != null && Convert.ToInt32(key.GetValue("UseFileTags", 0)) == 1;
            }
        }

        private static string DecodedTag(string field)
        {
            if (String.IsNullOrEmpty(field))
                return "";
            string result = Uri.UnescapeDataString(field);
            return result.Length > 1024 ? result.Substring(0, 1024) : result;
        }

        private static ArchiveEntryMetadata ReadEntryMetadata(string path, bool tagsEnabled)
        {
            var metadata = new ArchiveEntryMetadata
            {
                path = path,
                // The alias filename always exists, even for a solid archive
                // whose embedded tags are not yet available.
                title = Path.GetFileName(path)
            };
            if (!tagsEnabled)
                return metadata;

            try
            {
                string descriptor = Path.ChangeExtension(path, ".ast");
                if (!File.Exists(descriptor) || new FileInfo(descriptor).Length > 32768)
                    return metadata;
                string[] lines = File.ReadAllLines(descriptor, Encoding.UTF8);
                if (lines.Length < 2 || lines[0] != "ARCHIVE_STREAM_NATIVE_TRACK_V1")
                    return metadata;

                var output = new StringBuilder(8192);
                if (ReadNativeArchiveFileTags(lines[1], output, (uint)output.Capacity) != 1)
                    return metadata;
                string[] fields = output.ToString().Split('\t');
                if (fields.Length != 7)
                    return metadata;
                string embeddedTitle = DecodedTag(fields[0]);
                if (!String.IsNullOrWhiteSpace(embeddedTitle))
                {
                    metadata.title = embeddedTitle;
                    metadata.hasEmbeddedTitle = true;
                }
                metadata.artist = DecodedTag(fields[1]);
                metadata.album = DecodedTag(fields[2]);
                metadata.albumArtist = DecodedTag(fields[3]);
                metadata.number = fields[4];
                metadata.genre = DecodedTag(fields[5]);
                metadata.year = DecodedTag(fields[6]);
            }
            catch (Exception)
            {
                // Fail closed to the descriptor filename. In particular, an
                // older native add-on need not support this optional export.
            }
            return metadata;
        }

        // The SDK tag setter only reported pending writes for legacy aliases.
        // New versioned ID3 URLs are the reliable source of MusicBee metadata.
        // A background migration never navigates tabs, never adds deleted
        // entries, and checks that Clear List / new drops did not race it.
        private void ScheduleArchivePlaylistMetadata(string playlist, IEnumerable<string> tracks, bool sortRequested = false)
        {
#if AST_ARCHIVE_TAB_TEST
            return;
#else
            if (archiveDropClosing || archiveDropRoot == null || archiveDropRoot.IsDisposed ||
                String.IsNullOrWhiteSpace(playlist) || !NativeSupportsId3Alias())
                return;
            string[] initial = (tracks ?? Enumerable.Empty<string>()).ToArray();
            if (initial.Length == 0 || initial.Length > 10000)
                return;
            bool tagsEnabled = UseArchiveFileTags();
            string metadataSettings = CurrentArchiveMetadataSettings();
            Control root = archiveDropRoot;

            var thread = new Thread(() =>
            {
                var migrated = new string[initial.Length];
                int versioned = 0;
                int failed = 0;
                for (int i = 0; i < initial.Length; ++i)
                {
                    migrated[i] = initial[i];
                    if (archiveDropClosing)
                        break;
                    if (!IsPersistentNativeLibraryTrack(initial[i]))
                        continue;
                    try
                    {
                        // Metadata first: never traverse/decode archive frames
                        // merely to show titles in MusicBee. Cached lengths
                        // are still included without any decoder access.
                        string replacement = VersionedMusicBeeAlias(
                            Path.ChangeExtension(initial[i], ".ast"), tagsEnabled,
                            initial[i], false);
                        if (!String.Equals(replacement, initial[i], StringComparison.OrdinalIgnoreCase))
                        {
                            migrated[i] = replacement;
                            ++versioned;
                        }
                    }
                    catch (Exception error)
                    {
                        ++failed;
                        Trace("ARCHIVE_ID3_MIGRATION_ENTRY_FAIL type=" + error.GetType().Name);
                    }
                }
                if (archiveDropClosing || root.IsDisposed)
                    return;
                // Once on existing playlists, and when metadata settings
                // change, sort within each archive by embedded TRACKNUMBER.
                // Never intermingle tracks belonging to different archives.
                if (sortRequested && failed == 0)
                {
                    string[] ordered = OrderArchiveTracks(migrated);
                    if (!ordered.SequenceEqual(migrated, StringComparer.OrdinalIgnoreCase))
                        migrated = ordered;
                }
                bool sortedChanged = !migrated.SequenceEqual(initial, StringComparer.OrdinalIgnoreCase);
                Trace("ARCHIVE_TAB_ID3_SCAN checked=" + initial.Length + " versioned=" + versioned +
                    " ordered=" + (sortRequested ? "1" : "0") +
                    " failed=" + failed + " use_tags=" + (tagsEnabled ? "1" : "0"));
                if (!sortedChanged)
                {
                    if (sortRequested && failed == 0)
                        MarkArchivePlayerFirstSort();
                    return;
                }

                try
                {
                    root.BeginInvoke((Action)(() =>
                    {
                        if (archiveDropClosing || api.Playlist_QueryFilesEx == null ||
                            api.Playlist_SetFiles == null)
                            return;
                        string[] present;
                        if (!api.Playlist_QueryFilesEx(playlist, out present) || present == null ||
                            !present.SequenceEqual(initial, StringComparer.OrdinalIgnoreCase))
                        {
                            Trace("ARCHIVE_TAB_ID3_MIGRATION_SKIPPED reason=playlist_changed");
                            return;
                        }
                        // A second shared-settings change may arrive while a
                        // long archive metadata scan is still running. Never
                        // publish labels for a superseded preference snapshot.
                        if (!String.Equals(CurrentArchiveMetadataSettings(), metadataSettings,
                            StringComparison.Ordinal))
                        {
                            Trace("ARCHIVE_TAB_ID3_MIGRATION_SKIPPED reason=settings_changed_during_scan");
                            ScheduleArchivePlaylistMetadata(playlist, present, true);
                            return;
                        }
                        bool updated = api.Playlist_SetFiles(playlist, migrated);
                        Trace("ARCHIVE_TAB_ID3_MIGRATION_COMMIT success=" + (updated ? "1" : "0") +
                            " tracks=" + migrated.Length + " versioned=" + versioned +
                            " sorted=" + (sortRequested ? "1" : "0"));
                        if (updated && sortRequested && failed == 0)
                            MarkArchivePlayerFirstSort();
                        if (updated && api.MB_RefreshPanels != null)
                            api.MB_RefreshPanels();
                    }));
                }
                catch (InvalidOperationException) { }
            });
            thread.IsBackground = true;
            thread.Priority = ThreadPriority.BelowNormal;
            thread.Name = "Archive Stream MusicBee ID3 migration";
            thread.Start();
#endif
        }
    }
}
