// SDK-independent UI integration regression for Archive Stream's optional
// verified-duration window. Uses a synthetic metadata-only alias and a
// duration cache, never opens/decodes an audio archive.
using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Reflection;
using System.Windows.Forms;
using System.Drawing;
using MusicBeePlugin;

internal static class VerifiedViewHarness
{
    private const BindingFlags PrivateInstance = BindingFlags.NonPublic | BindingFlags.Instance;
    private static void Put(Plugin plugin, string name, object value)
    {
        var field = typeof(Plugin).GetField(name, PrivateInstance);
        if (field == null)
            throw new Exception("Plugin field missing: " + name);
        field.SetValue(plugin, value);
    }

    private static void Frame(Stream stream, string name, string value)
    {
        byte[] unicode = Encoding.Unicode.GetBytes(value);
        int length = 3 + unicode.Length;
        byte[] header = Encoding.ASCII.GetBytes(name);
        stream.Write(header, 0, header.Length);
        stream.WriteByte((byte)(length >> 24)); stream.WriteByte((byte)(length >> 16));
        stream.WriteByte((byte)(length >> 8)); stream.WriteByte((byte)length);
        stream.WriteByte(0); stream.WriteByte(0);
        stream.WriteByte(1); stream.WriteByte(255); stream.WriteByte(254);
        stream.Write(unicode, 0, unicode.Length);
    }

    [STAThread]
    public static int Main()
    {
        string root = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "ArchiveStreamPlayer", "MusicBee", "v", "qa-view-" + Guid.NewGuid().ToString("N"));
        string aliases = Path.Combine(root, "id3-qa");
        Directory.CreateDirectory(aliases);
        string archive = Path.Combine(root, "synthetic.zip");
        string descriptor = Path.Combine(aliases, "entry-00000017.ast");
        string alias = Path.ChangeExtension(descriptor, ".mp3");
        Form window = null;
        try
        {
            File.WriteAllBytes(archive, new byte[] { 0, 1, 2, 3 });
            byte[] manifest = new UTF8Encoding(false).GetBytes(
                "ARCHIVE_STREAM_NATIVE_TRACK_V1\n" +
                archive + ".archive_stream|17|02 - Whitebear - Fever Dreams.mp3\n");
            File.WriteAllBytes(descriptor, manifest);
            using (var frames = new MemoryStream())
            {
                Frame(frames, "TIT2", "Fever Dreams");
                Frame(frames, "TPE1", "Whitebear");
                Frame(frames, "TALB", "Elementis");
                Frame(frames, "TRCK", "2");
                Frame(frames, "TCON", "Psytrance");
                byte[] metadata = frames.ToArray();
                using (var output = new MemoryStream())
                {
                    output.Write(Encoding.ASCII.GetBytes("ID3"), 0, 3);
                    output.WriteByte(3); output.WriteByte(0); output.WriteByte(0);
                    output.WriteByte((byte)((metadata.Length >> 21) & 127));
                    output.WriteByte((byte)((metadata.Length >> 14) & 127));
                    output.WriteByte((byte)((metadata.Length >> 7) & 127));
                    output.WriteByte((byte)(metadata.Length & 127));
                    output.Write(metadata, 0, metadata.Length);
                    output.Write(manifest, 0, manifest.Length);
                    File.WriteAllBytes(alias, output.ToArray());
                }
            }
            FileInfo source = new FileInfo(archive);
            File.WriteAllText(Path.Combine(root, "duration-00000017.txt"),
                source.Length + "|" + source.LastWriteTimeUtc.Ticks + "|352918");

            var api = new Plugin.MusicBeeApiInterface();
            string playlist = Path.Combine(root, "Archive Stream Player", "Archive Player.mbp");
            int next = 0;
            int playlistChanges = 0;
            api.Playlist_QueryPlaylists = () => { next = 0; return true; };
            api.Playlist_QueryGetNextPlaylist = () => next++ == 0 ? playlist : null;
            api.Playlist_GetName = url => "Archive Stream Player\\Archive Player";
            api.Playlist_QueryFilesEx = (string url, out string[] files) =>
            {
                files = new[] { alias };
                return url == playlist;
            };
            api.Playlist_SetFiles = (url, files) => { ++playlistChanges; return true; };
            api.NowPlayingList_PlayNow = url => url == alias;

            var plugin = new Plugin();
            Put(plugin, "api", api);
            var show = typeof(Plugin).GetMethod("ShowArchiveVerifiedView", PrivateInstance);
            var close = typeof(Plugin).GetMethod("CloseArchiveVerifiedView", PrivateInstance);
            show.Invoke(plugin, new object[] { null, EventArgs.Empty });
            window = (Form)typeof(Plugin).GetField("archiveVerifiedView", PrivateInstance).GetValue(plugin);
            DataGridView grid = (DataGridView)typeof(Plugin).GetField("archiveVerifiedGrid", PrivateInstance).GetValue(plugin);
            if (window == null || !window.Visible || grid == null || grid.Rows.Count != 1 ||
                grid.Columns.Count != 6 || !window.ShowInTaskbar || window.TopMost || window.Owner != null)
                throw new Exception("Verified view must be an independent, visible window with 6 columns");
            if (grid.Columns["Duration"].DisplayIndex != 1 ||
                Convert.ToString(grid.Rows[0].Cells["Artist"].Value) != "Whitebear" ||
                Convert.ToString(grid.Rows[0].Cells["Title"].Value) != "Fever Dreams" ||
                Convert.ToString(grid.Rows[0].Cells["Album"].Value) != "Elementis" ||
                Convert.ToString(grid.Rows[0].Cells["Duration"].Value) != "5:52")
                throw new Exception("Time, tag or column mapping incorrect");
            show.Invoke(plugin, new object[] { null, EventArgs.Empty });
            var reused = (Form)typeof(Plugin).GetField("archiveVerifiedView", PrivateInstance).GetValue(plugin);
            if (!Object.ReferenceEquals(window, reused))
                throw new Exception("Reopening duplicated verified view");
            if (playlistChanges != 0)
                throw new Exception("Verified view modified MusicBee's native playlist");
            close.Invoke(plugin, new object[0]);
            if (!window.IsDisposed)
                throw new Exception("Verified view was not disposed");
            Console.WriteLine("ARCHIVE_VERIFIED_VIEW_TEST_PASS time=5:52 tags=correct independent_window=1 " +
                "native_tabs_untouched=1 duplicate_views=0 cleanup=1");
            return 0;
        }
        finally
        {
            if (window != null && !window.IsDisposed)
                window.Close();
            try { Directory.Delete(root, true); } catch { }
        }
    }
}
