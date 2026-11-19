// MusicBee's supported native playlist tab is the archive browser.
// Unlike an MB_AddPanel overlay, navigating to a playlist creates/selects a
// genuine MusicBee tab, while MusicBee retains normal playback and controls.
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private const string ArchivePlayerPlaylistName = "Archive Player";
        private const string ArchivePlayerPlaylistFolder = "Archive Stream Player";
        private bool archivePlayerStartupScheduled;
        private bool archivePlaylistGuardRunning;

        // Store migration state independently of MusicBee's playlist: clearing
        // or deleting the playlist must never silently restore old tracks.
        private static string ArchivePlayerMigrationMarker()
        {
#if AST_ARCHIVE_TAB_TEST
            return Path.Combine(Path.GetTempPath(), "archive_stream_tab_test_migration.txt");
#else
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ArchiveStreamPlayer", "MusicBee", "archive-player-migrated-v1.txt");
#endif
        }

        private static bool ArchivePlayerWasMigrated()
        {
            return File.Exists(ArchivePlayerMigrationMarker());
        }

        private static void MarkArchivePlayerMigrated()
        {
            string marker = ArchivePlayerMigrationMarker();
            Directory.CreateDirectory(Path.GetDirectoryName(marker));
            if (!File.Exists(marker))
                File.WriteAllText(marker, "1");
        }

        private string FindArchivePlayerPlaylist()
        {
            if (api.Playlist_QueryPlaylists == null || api.Playlist_QueryGetNextPlaylist == null)
                return null;
            if (!api.Playlist_QueryPlaylists())
                return null;

            string candidate;
            for (int index = 0; index < 100000; ++index)
            {
                candidate = api.Playlist_QueryGetNextPlaylist();
                if (String.IsNullOrWhiteSpace(candidate))
                    break;

                string name = api.Playlist_GetName == null ? "" : api.Playlist_GetName(candidate);
                string normalized = candidate.Replace('/', '\\');
                string ownershipMarker = "\\" + ArchivePlayerPlaylistFolder + "\\";
                // MusicBee returns folder-qualified playlist names, e.g.
                // "Archive Stream Player\\Archive Player", not only the title.
                bool named = String.Equals(name, ArchivePlayerPlaylistName, StringComparison.OrdinalIgnoreCase) ||
                    String.Equals(name, ArchivePlayerPlaylistFolder + "\\" + ArchivePlayerPlaylistName,
                        StringComparison.OrdinalIgnoreCase);
                bool owned = normalized.IndexOf(ownershipMarker, StringComparison.OrdinalIgnoreCase) >= 0;
                if (named && owned)
                    return candidate;
            }
            return null;
        }

        private static string[] UniqueArchivePlayerTracks(IEnumerable<string> tracks)
        {
            return (tracks ?? Enumerable.Empty<string>())
                .Where(IsPersistentNativeLibraryTrack)
                .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
        }

        // Restrict our playlist to references allocated by Archive Stream.
        // Do not require an existing .ast here: disconnected/offline archive
        // references are user playlist data, not grounds for deleting a row.
        private static bool IsArchivePlayerOwnedUrl(string path)
        {
            if (String.IsNullOrWhiteSpace(path))
                return false;
            try
            {
                string full = Path.GetFullPath(path);
                string root = Path.Combine(
                    Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                    "ArchiveStreamPlayer", "MusicBee", "v") + Path.DirectorySeparatorChar;
                return full.StartsWith(root, StringComparison.OrdinalIgnoreCase) &&
                    full.EndsWith(".mp3", StringComparison.OrdinalIgnoreCase);
            }
            catch (Exception)
            {
                return false;
            }
        }

        // MusicBee can append an ordinary MP3 through its own DragDrop handler
        // when any playlist tab has focus. It never passes through our archive
        // drag filter. Remove only those foreign references from our playlist;
        // never touch the original file, the Music library, or Now Playing.
        // All SDK calls must execute on MusicBee's UI thread.
        private void GuardArchivePlayerPlaylist()
        {
            if (archivePlaylistGuardRunning || archiveDropClosing ||
                api.Playlist_QueryFilesEx == null || api.Playlist_SetFiles == null)
                return;

            archivePlaylistGuardRunning = true;
            try
            {
                string playlist = FindArchivePlayerPlaylist();
                if (String.IsNullOrWhiteSpace(playlist))
                    return;
                string[] current;
                if (!api.Playlist_QueryFilesEx(playlist, out current) || current == null)
                    return;
                string[] owned = current.Where(IsArchivePlayerOwnedUrl).ToArray();
                if (owned.Length == current.Length)
                    return;

                // Do not overwrite an intervening drop or Clear List event.
                string[] stillCurrent;
                if (!api.Playlist_QueryFilesEx(playlist, out stillCurrent) || stillCurrent == null ||
                    !stillCurrent.SequenceEqual(current, StringComparer.OrdinalIgnoreCase))
                {
                    Trace("ARCHIVE_PLAYLIST_GUARD_SKIPPED reason=list_changed");
                    return;
                }
                bool updated = api.Playlist_SetFiles(playlist, owned);
                Trace("ARCHIVE_PLAYLIST_GUARD updated=" + (updated ? "1" : "0") +
                    " removed_nonarchive=" + (current.Length - owned.Length) +
                    " archive_entries=" + owned.Length);
                if (updated && api.MB_RefreshPanels != null)
                    api.MB_RefreshPanels();
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_PLAYLIST_GUARD_FAILED type=" + error.GetType().Name);
            }
            finally
            {
                archivePlaylistGuardRunning = false;
            }
        }

        private string[] PreviouslyImportedArchiveTracks()
        {
            if (api.Library_QueryFilesEx == null)
                return new string[0];

            try
            {
                string[] all;
                if (!api.Library_QueryFilesEx("domain=Library", out all) || all == null)
                    return new string[0];
                string[] existing = UniqueArchivePlayerTracks(all);
                Trace("ARCHIVE_TAB_MIGRATE_SCAN existing=" + existing.Length);
                return existing;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_TAB_MIGRATE_SCAN_FAIL type=" + error.GetType().Name);
                return new string[0];
            }
        }

        // Run only on MusicBee's UI thread. Never mutate its database directly.
        private bool OpenArchivePlayerTab(IEnumerable<string> incoming)
        {
            if (archiveDropClosing ||
                api.Playlist_CreatePlaylist == null || api.Playlist_AppendFiles == null)
            {
                Trace("ARCHIVE_TAB_UNAVAILABLE reason=missing_sdk_or_shutting_down");
                return false;
            }

            string[] tracks = UniqueArchivePlayerTracks(incoming);
            try
            {
                string playlist = FindArchivePlayerPlaylist();
                bool created = false;
                if (String.IsNullOrWhiteSpace(playlist))
                {
                    // First installation: expose archive entries already imported
                    // into MusicBee, without rescanning or extracting their archives.
                    string[] oldTracks = ArchivePlayerWasMigrated() ?
                        new string[0] : PreviouslyImportedArchiveTracks();
                    string[] seed = UniqueArchivePlayerTracks(oldTracks.Concat(tracks));
                    playlist = api.Playlist_CreatePlaylist(ArchivePlayerPlaylistFolder,
                        ArchivePlayerPlaylistName, seed);
                    created = !String.IsNullOrWhiteSpace(playlist);
                    if (!created)
                    {
                        Trace("ARCHIVE_TAB_CREATE_FAILED seed=" + seed.Length);
                        return false;
                    }
                    Trace("ARCHIVE_TAB_CREATED tracks=" + seed.Length);
                }

                // The user can clear the list; never re-migrate the entire
                // MusicBee library after the first successful playlist creation.
                MarkArchivePlayerMigrated();

                if (!created && tracks.Length != 0)
                {
                    string[] present = null;
                    bool queried = api.Playlist_QueryFilesEx != null &&
                        api.Playlist_QueryFilesEx(playlist, out present);
                    if (!queried)
                    {
                        // Avoid a possible duplicate if MusicBee cannot query its
                        // existing playlist. Leave the current list unmodified.
                        Trace("ARCHIVE_TAB_APPEND_SKIPPED reason=playlist_query_failed");
                    }
                    else
                    {
                        var known = new HashSet<string>(present ?? new string[0],
                            StringComparer.OrdinalIgnoreCase);
                        string[] additions = tracks.Where(path => !known.Contains(path)).ToArray();
                        if (additions.Length != 0)
                        {
                            if (!api.Playlist_AppendFiles(playlist, additions))
                            {
                                Trace("ARCHIVE_TAB_APPEND_FAILED tracks=" + additions.Length);
                                return false;
                            }
                        }
                        Trace("ARCHIVE_TAB_UPDATED added=" + additions.Length +
                            " existing=" + known.Count);
                    }
                }

                // IMPORTANT: Command.NavigateTo does NOT select a native
                // playlist tab using an .mbp file path. MusicBee treats that
                // path as a document: it can replace the current Music tab's
                // destination, or ask Windows for an .mbp file association.
                // Never pass the .mbp URL to NavigateTo. Updating the backing
                // playlist is safe; the host tab is selected by the user.
                Trace("ARCHIVE_PLAYLIST_READY created=" + (created ? "1" : "0") +
                    " incoming=" + tracks.Length + " navigation=host_owned");
                StartPlaybackDiagnostics();
                GuardArchivePlayerPlaylist();
                RefreshArchiveVerifiedView();
                if (api.Playlist_QueryFilesEx != null)
                {
                    string[] present;
                    if (api.Playlist_QueryFilesEx(playlist, out present))
                        ScheduleArchivePlaylistMetadata(playlist, present ?? new string[0]);
                }
                return true;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_TAB_FAILED type=" + error.GetType().Name);
                return false;
            }
        }

        private void EnsureArchivePlayerPlaylist()
        {
            if (api.Playlist_CreatePlaylist == null)
                return;
            string[] seed = ArchivePlayerWasMigrated() ?
                new string[0] : PreviouslyImportedArchiveTracks();
            string playlist = api.Playlist_CreatePlaylist(ArchivePlayerPlaylistFolder,
                ArchivePlayerPlaylistName, seed);
            if (!String.IsNullOrWhiteSpace(playlist))
            {
                MarkArchivePlayerMigrated();
                Trace("ARCHIVE_TAB_STARTUP_CREATED tracks=" + seed.Length);
            }
        }

        private void ScheduleArchivePlayerStartup()
        {
            if (archivePlayerStartupScheduled || archiveDropClosing || archiveDropRoot == null ||
                archiveDropRoot.IsDisposed)
                return;
            archivePlayerStartupScheduled = true;
            try
            {
                // Startup must not navigate to an .mbp URL: MusicBee can still
                // be restoring tabs, causing duplicate tab titles or Windows'
                // "choose an app to open .mbp" dialog. Create the backing
                // playlist if absent; archive drops or the Tools command
                // explicitly activate/reopen the real tab.
                archiveDropRoot.BeginInvoke((Action)(() =>
                {
                    if (archiveDropClosing)
                        return;
                    try
                    {
                        string playlist = FindArchivePlayerPlaylist();
                        if (String.IsNullOrWhiteSpace(playlist))
                        {
                            EnsureArchivePlayerPlaylist();
                            playlist = FindArchivePlayerPlaylist();
                        }
                        else
                        {
                            MarkArchivePlayerMigrated();
                        }

                        // Keep a host-owned playlist archive-only, including
                        // ordinary MP3s MusicBee may append through its own
                        // drop handlers. Never alter other playlists or library.
                        GuardArchivePlayerPlaylist();

                        // Metadata repair may run at low priority on startup,
                        // but never navigate/open tabs or repopulate a cleared
                        // playlist. This also applies new Use File Tags prefs.
                        if (!String.IsNullOrWhiteSpace(playlist) && api.Playlist_QueryFilesEx != null)
                        {
                            string[] existing = null;
                            if (api.Playlist_QueryFilesEx(playlist, out existing))
                                ScheduleArchivePlaylistMetadata(playlist, existing ?? new string[0], ArchivePlayerNeedsFirstSort());
                        }
                    }
                    catch (Exception error)
                    {
                        Trace("ARCHIVE_TAB_STARTUP_CHECK_FAIL type=" + error.GetType().Name);
                    }
                }));
            }
            catch (Exception error)
            {
                archivePlayerStartupScheduled = false;
                Trace("ARCHIVE_TAB_STARTUP_FAILED type=" + error.GetType().Name);
            }
        }

        private void OnOpenArchivePlayer(object sender, EventArgs args)
        {
            if (!OpenArchivePlayerTab(new string[0]))
            {
                MessageBox.Show("MusicBee could not find or create the Archive Player playlist.",
                    "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            MessageBox.Show("Archive Player is ready. Select its existing tab, or open the playlist " +
                "from MusicBee's Playlists navigator if you previously closed the tab.",
                "Archive Stream Player", MessageBoxButtons.OK, MessageBoxIcon.Information);
        }
    }
}
