using System;
using System.Collections.Generic;
using System.Linq;
using System.IO;
using System.Text;
using System.Windows.Forms;

namespace MusicBeePlugin
{
    public enum Command { NavigateTo = 1 }

    public delegate bool QueryFiles(string query, out string[] files);
    public delegate bool QueryPlaylist(string url, out string[] files);

    public class FakeApi
    {
        public Func<bool> Playlist_QueryPlaylists;
        public Func<string> Playlist_QueryGetNextPlaylist;
        public Func<string, string> Playlist_GetName;
        public Func<string, string, string[], string> Playlist_CreatePlaylist;
        public Func<string, string[], bool> Playlist_AppendFiles;
        public Func<string, string[], bool> Playlist_SetFiles;
        public Action MB_RefreshPanels;
        public QueryPlaylist Playlist_QueryFilesEx;
        public QueryFiles Library_QueryFilesEx;
        public Func<Command, object, bool> MB_InvokeCommand;
    }

    public partial class Plugin
    {
        private FakeApi api = new FakeApi();
        private bool archiveDropClosing;
        private Control archiveDropRoot;
        private static bool IsPersistentNativeLibraryTrack(string p)
        {
            return !String.IsNullOrWhiteSpace(p) && p.EndsWith(".mp3", StringComparison.OrdinalIgnoreCase);
        }
        private static void Trace(string message)
        {
            if (message.StartsWith("ARCHIVE_TAB_"))
                Console.WriteLine(message);
        }
        private void StartPlaybackDiagnostics() {}
        private void ScheduleArchivePlaylistMetadata(string playlist, IEnumerable<string> tracks, bool sortRequested = false) {}
        private static int ReadNativeArchiveFileTags(string virtualPath, StringBuilder output, uint capacity)
        {
            int number = 0;
            if (virtualPath.Contains("|7|"))
                number = 3;
            else if (virtualPath.Contains("|8|"))
                number = 1;
            else if (virtualPath.Contains("|9|"))
                number = 2;
            if (number == 0)
                return 0;
            output.Append("Song\tArtist\tAlbum\t\t").Append(number).Append("\tGenre\t2026");
            return 1;
        }

        private static void VerifyArchiveOrder()
        {
            string folder = Path.Combine(Path.GetTempPath(), "ast-sort-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(folder);
            try
            {
                string[] paths = new string[5];
                string[] names = { "ten", "two", "one", "other-ten", "other-two" };
                string[] virtuals = {
                    @"X:\Audio\album.rar.archive_stream|7|10 - Ten.mp3",
                    @"X:\Audio\album.rar.archive_stream|8|02 - Two.mp3",
                    @"X:\Audio\album.rar.archive_stream|9|01 - One.mp3",
                    @"X:\Audio\second.zip.archive_stream|10|10 - Ten.mp3",
                    @"X:\Audio\second.zip.archive_stream|11|02 - Two.mp3"
                };
                for (int i = 0; i < paths.Length; ++i)
                {
                    paths[i] = Path.Combine(folder, names[i] + ".mp3");
                    File.WriteAllText(Path.ChangeExtension(paths[i], ".ast"),
                        "ARCHIVE_STREAM_NATIVE_TRACK_V1\n" + virtuals[i] + "\n");
                }
                string[] sorted = OrderArchiveTracks(paths, true);
                string[] expected = { paths[1], paths[2], paths[0], paths[4], paths[3] };
                if (!sorted.SequenceEqual(expected, StringComparer.OrdinalIgnoreCase))
                    throw new Exception("Embedded TRACKNUMBER or filename fallback order is incorrect");
                if (!OrderArchiveTracks(sorted, true).SequenceEqual(expected, StringComparer.OrdinalIgnoreCase))
                    throw new Exception("Track ordering is not stable");
                Console.WriteLine("ARCHIVE_PLAYER_SORT_TEST_PASS embedded_track_numbers=1 filename_fallback=1 independent_archive_groups=1");
            }
            finally
            {
                Directory.Delete(folder, true);
            }
        }

        private static readonly string Url = @"C:\MusicBee\Playlists\Archive Stream Player\Archive Player.mbp";
        private static readonly string AliasRoot = System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ArchiveStreamPlayer", "MusicBee", "v", "sdk-harness");
        private static readonly string Old = System.IO.Path.Combine(AliasRoot, "entry-00000001.mp3");
        private static readonly string New = System.IO.Path.Combine(AliasRoot, "entry-00000002.mp3");
        private readonly List<string> files = new List<string>();
        private bool playlistExists;
        private int queryIndex;
        private int creations;
        private int navigations;

        private Plugin()
        {
            api.Playlist_QueryPlaylists = () => { queryIndex = 0; return true; };
            api.Playlist_QueryGetNextPlaylist = () =>
            {
                if (playlistExists && queryIndex++ == 0)
                    return Url;
                queryIndex = 0;
                return null;
            };
            api.Playlist_GetName = url => @"Archive Stream Player\Archive Player";
            api.Playlist_CreatePlaylist = (folder, title, items) =>
            {
                if (folder != "Archive Stream Player" || title != "Archive Player")
                    throw new Exception("Incorrect playlist identity");
                creations++;
                playlistExists = true;
                files.Clear();
                files.AddRange(items);
                return Url;
            };
            api.Playlist_QueryFilesEx = (string url, out string[] items) =>
            {
                items = files.ToArray();
                return playlistExists && url == Url;
            };
            api.Playlist_AppendFiles = (url, items) =>
            {
                if (url != Url)
                    return false;
                files.AddRange(items);
                return true;
            };
            api.Playlist_SetFiles = (url, items) =>
            {
                if (url != Url)
                    return false;
                files.Clear();
                files.AddRange(items);
                return true;
            };
            api.MB_RefreshPanels = () => {};
            api.Library_QueryFilesEx = (string query, out string[] items) =>
            {
                items = new[] { Old, Old, "ignored.txt" };
                return true;
            };
            // Any NavigateTo call with .mbp is a regression: it opens the
            // Windows association picker and hijacks another MusicBee tab.
            api.MB_InvokeCommand = (command, obj) =>
            {
                navigations++;
                throw new Exception("Unsafe NavigateTo was called.");
            };
        }
        public static int Main()
        {
            VerifyArchiveOrder();
            string marker = Plugin.ArchivePlayerMigrationMarker();
            if (System.IO.File.Exists(marker))
                System.IO.File.Delete(marker);
            var plugin = new Plugin();
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.creations != 1 ||
                plugin.files.Count != 1 || plugin.files[0] != Old || plugin.navigations != 0)
                throw new Exception("First-run migration failed");

            if (!plugin.OpenArchivePlayerTab(new[] { New, New }) ||
                plugin.creations != 1 || plugin.files.Count != 2)
                throw new Exception("Append/deduplication failed");

            if (!plugin.OpenArchivePlayerTab(new[] { New }) ||
                plugin.creations != 1 || plugin.files.Count != 2 || plugin.navigations != 0)
                throw new Exception("Existing playlist reuse failed");

            if (!plugin.OpenArchivePlayerTab(new string[0]) ||
                plugin.creations != 1 || plugin.navigations != 0)
                throw new Exception("Unsafe playlist navigation attempted");

            // MusicBee's own drag handler may append a regular MP3. The guard
            // must remove it without clearing any native archive references.
            plugin.files.Add(@"M:\Music\Ordinary-Song.mp3");
            plugin.GuardArchivePlayerPlaylist();
            if (plugin.files.Count != 2 || plugin.files[0] != Old ||
                plugin.files[1] != New || plugin.navigations != 0)
                throw new Exception("Ordinary MP3 polluted Archive Player");

            plugin.files.Clear(); // User explicitly clicked Clear List.
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.files.Count != 0)
                throw new Exception("Cleared playlist resurrected archived tracks");
            plugin.playlistExists = false; // User also removed backing playlist.
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.files.Count != 0)
                throw new Exception("Deleted playlist migrated historical tracks again");
            if (System.IO.File.Exists(marker))
                System.IO.File.Delete(marker);
            Console.WriteLine("ARCHIVE_PLAYER_TAB_TEST_PASS creates=2 tracks=0 cleared_stays_cleared=1 unsafe_navigations=0 ordinary_mp3_removed=1");
            return 0;
        }
    }
}
