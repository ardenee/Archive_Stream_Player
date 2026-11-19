using System;
using System.Collections.Generic;
using System.Linq;
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
        private void ScheduleArchivePlaylistMetadata(string playlist, IEnumerable<string> tracks) {}

        private static readonly string Url = @"C:\MusicBee\Playlists\Archive Stream Player\Archive Player.mbp";
        private readonly List<string> files = new List<string>();
        private bool playlistExists;
        private bool tabOpen;
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
            api.Library_QueryFilesEx = (string query, out string[] items) =>
            {
                items = new[] { "legacy.mp3", "legacy.mp3", "ignored.txt" };
                return true;
            };
            api.MB_InvokeCommand = (command, obj) =>
            {
                if (command != Command.NavigateTo || (string)obj != Url)
                    return false;
                tabOpen = true;
                navigations++;
                return true;
            };
        }
        public static int Main()
        {
            string marker = Plugin.ArchivePlayerMigrationMarker();
            if (System.IO.File.Exists(marker))
                System.IO.File.Delete(marker);
            var plugin = new Plugin();
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.creations != 1 ||
                plugin.files.Count != 1 || plugin.files[0] != "legacy.mp3")
                throw new Exception("First-run migration failed");

            if (!plugin.OpenArchivePlayerTab(new[] { "new.mp3", "new.mp3" }) ||
                plugin.creations != 1 || plugin.files.Count != 2)
                throw new Exception("Append/deduplication failed");

            plugin.tabOpen = false;
            if (!plugin.OpenArchivePlayerTab(new[] { "new.mp3" }) ||
                !plugin.tabOpen || plugin.creations != 1 || plugin.files.Count != 2)
                throw new Exception("Closed-tab reuse failed");

            if (!plugin.OpenArchivePlayerTab(new string[0]) ||
                plugin.creations != 1 || plugin.navigations != 4)
                throw new Exception("Repeated navigation failed");

            plugin.files.Clear(); // User explicitly clicked Clear List.
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.files.Count != 0)
                throw new Exception("Cleared playlist resurrected archived tracks");
            plugin.playlistExists = false; // User also removed backing playlist.
            if (!plugin.OpenArchivePlayerTab(new string[0]) || plugin.files.Count != 0)
                throw new Exception("Deleted playlist migrated historical tracks again");
            if (System.IO.File.Exists(marker))
                System.IO.File.Delete(marker);
            Console.WriteLine("ARCHIVE_PLAYER_TAB_TEST_PASS creates=2 tracks=0 cleared_stays_cleared=1 tab_reopened=1");
            return 0;
        }
    }
}
