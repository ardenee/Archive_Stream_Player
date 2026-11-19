// Deterministic ordering of archive entries, without relying on archive
// enumeration order or the user's MusicBee tab layout. Embedded TRACKNUMBER
// wins; a numeric filename prefix is the fallback. Archives retain their
// group position in the playlist; never intermingle different albums.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private sealed class ArchiveOrderKey
        {
            public string path;
            public string archive;
            public string member;
            public int track;
            public int ordinal;
        }

        private static bool TryReadArchiveEntryPath(string alias, out string archive, out string member, out string virtualPath)
        {
            archive = "";
            member = "";
            virtualPath = "";
            try
            {
                string descriptor = Path.ChangeExtension(alias, ".ast");
                if (!File.Exists(descriptor) || new FileInfo(descriptor).Length > 16384)
                    return false;
                string[] lines = File.ReadAllLines(descriptor, Encoding.UTF8);
                if (lines.Length != 2 || lines[0] != "ARCHIVE_STREAM_NATIVE_TRACK_V1")
                    return false;
                virtualPath = lines[1].TrimEnd('\r');
                const string marker = ".archive_stream|";
                int mark = virtualPath.IndexOf(marker, StringComparison.OrdinalIgnoreCase);
                if (mark <= 0)
                    return false;
                int divider = virtualPath.IndexOf('|', mark + marker.Length);
                if (divider < 0 || divider + 1 == virtualPath.Length)
                    return false;
                archive = virtualPath.Substring(0, mark);
                member = virtualPath.Substring(divider + 1);
                return true;
            }
            catch (Exception)
            {
                return false;
            }
        }

        // Natural track number only when it is a leading numeric token in
        // the entry basename, e.g. "03 - Song.mp3", "3_Song.ogg".
        // A year in a parent archive/folder is never treated as track no.
        private static int FilenameTrackNumber(string member)
        {
            string name = Path.GetFileName(member.Replace('/', '\\'));
            int at = 0;
            while (at < name.Length && Char.IsWhiteSpace(name[at]))
                ++at;
            int start = at;
            while (at < name.Length && at - start < 4 && name[at] >= '0' && name[at] <= '9')
                ++at;
            int track;
            if (at == start || at == name.Length ||
                (name[at] != ' ' && name[at] != '-' && name[at] != '_' && name[at] != '.'))
                return 0;
            if (!Int32.TryParse(name.Substring(start, at - start), NumberStyles.None,
                CultureInfo.InvariantCulture, out track))
                return 0;
            return track > 0 && track <= 999 ? track : 0;
        }

        private static ArchiveOrderKey MakeArchiveOrderKey(string path, int ordinal, bool probeTags)
        {
            var key = new ArchiveOrderKey { path = path, ordinal = ordinal, archive = path, member = path };
            string virtualPath;
            if (!TryReadArchiveEntryPath(path, out key.archive, out key.member, out virtualPath))
                return key;
            key.track = FilenameTrackNumber(key.member);
            if (!probeTags)
                return key;
            try
            {
                var output = new StringBuilder(8192);
                if (ReadNativeArchiveFileTags(virtualPath, output, (uint)output.Capacity) == 1)
                {
                    string[] fields = output.ToString().Split('\t');
                    int tagged;
                    if (fields.Length == 7 &&
                        Int32.TryParse(fields[4], NumberStyles.Integer, CultureInfo.InvariantCulture, out tagged) &&
                        tagged > 0 && tagged <= 9999)
                        key.track = tagged;
                }
            }
            catch (Exception)
            {
                // Older add-ons and unsupported/solid archives use names.
            }
            return key;
        }

        private static string[] OrderArchiveTracks(IEnumerable<string> paths, bool probeTags = true)
        {
            string[] items = (paths ?? Enumerable.Empty<string>()).ToArray();
            if (items.Length <= 1)
                return items;
            // Avoid a huge sequential metadata scan merely to order a large
            // playlist. Filename fallback is deterministic for all sizes.
            if (items.Length > 2000)
                probeTags = false;

            var keys = items.Select((path, index) => MakeArchiveOrderKey(path, index, probeTags)).ToArray();
            var groups = keys.GroupBy(k => k.archive, StringComparer.OrdinalIgnoreCase);
            return groups.SelectMany(group => group
                .OrderBy(k => k.track == 0 ? 1 : 0)
                .ThenBy(k => k.track == 0 ? Int32.MaxValue : k.track)
                .ThenBy(k => k.member, StringComparer.OrdinalIgnoreCase)
                .ThenBy(k => k.ordinal)
                .Select(k => k.path)).ToArray();
        }

        private static bool ArchivePlayerNeedsFirstSort()
        {
#if AST_ARCHIVE_TAB_TEST
            return true;
#else
            return !File.Exists(Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ArchiveStreamPlayer", "MusicBee", "archive-order-v1.txt"));
#endif
        }

        private static void MarkArchivePlayerFirstSort()
        {
#if !AST_ARCHIVE_TAB_TEST
            string marker = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ArchiveStreamPlayer", "MusicBee", "archive-order-v1.txt");
            Directory.CreateDirectory(Path.GetDirectoryName(marker));
            if (!File.Exists(marker))
                File.WriteAllText(marker, "Sorted existing archive playlist by tag number (when available), then filename");
#endif
        }
    }
}
