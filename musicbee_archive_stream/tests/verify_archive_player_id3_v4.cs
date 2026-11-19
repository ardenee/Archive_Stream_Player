using System;
using System.IO;
using System.Linq;
using System.Text;

namespace MusicBeePlugin
{
    // Standalone managed contract harness. No installed MusicBee process,
    // archive extraction, native audio backend, or user library is required.
    public partial class Plugin
    {
        private static readonly object persistentTrackLock = new object();

        private sealed class ArchiveEntryMetadata
        {
            public string title, artist, album, albumArtist, number, genre, year;
            public bool hasEmbeddedTitle;
        }

        private static ArchiveEntryMetadata ReadEntryMetadata(string path, bool tagsEnabled)
        {
            return new ArchiveEntryMetadata
            {
                title = "Fever Dreams",
                hasEmbeddedTitle = true,
                artist = "Whitebear",
                album = "Elementis",
                albumArtist = "VA",
                number = "2",
                genre = "Psytrance",
                year = "2013"
            };
        }

        private static int FormatNativeTitle(string virtualPath, StringBuilder output, uint capacity)
        {
            output.Append("02 - Whitebear - Fever Dreams.mp3");
            return 1;
        }

        private static long QueryArchiveDurationMs(string alias, bool allowDecode = true)
        {
            return allowDecode ? 352918 : 0; // Stub: initial metadata must not probe decoder.
        }

        private static bool CreateNativeDescriptorHardLink(string alias, string descriptor, IntPtr reserved)
        {
            throw new Exception("The metadata alias must never hard-link its descriptor.");
        }

        private static void Trace(string message) {}

        private static bool SameBytes(byte[] left, byte[] right)
        {
            return left.SequenceEqual(right);
        }

        private static void VerifyFrameSet(string alias, bool tagsEnabled, bool withDuration)
        {
            byte[] bytes = File.ReadAllBytes(alias);
            byte[] manifest = File.ReadAllBytes(Path.ChangeExtension(alias, ".ast"));
            if (Encoding.ASCII.GetString(bytes, 0, 6) != "ID3\x03\x00\x00")
                throw new Exception("Expected ID3v2.3 metadata alias.");
            int tagLength = 10 + (bytes[6] << 21) + (bytes[7] << 14) + (bytes[8] << 7) + bytes[9];
            if (tagLength + manifest.Length != bytes.Length)
                throw new Exception("Manifest trailer is not exact.");
            if (!SameBytes(bytes.Skip(tagLength).ToArray(), manifest))
                throw new Exception("Manifest trailer bytes changed.");
            bool title = false, artist = false, album = false, duration = false;
            int offset = 10;
            while (offset < tagLength)
            {
                if (offset + 10 > tagLength)
                    throw new Exception("Incomplete frame header.");
                string field = Encoding.ASCII.GetString(bytes, offset, 4);
                int size = (bytes[offset + 4] << 24) | (bytes[offset + 5] << 16) |
                    (bytes[offset + 6] << 8) | bytes[offset + 7];
                if (size < 2 || offset + 10 + size > tagLength)
                    throw new Exception("Malformed ID3 frame.");
                if (!new[] { "TIT2", "TLEN", "TPE1", "TALB", "TPE2", "TRCK", "TCON", "TYER" }.Contains(field))
                    throw new Exception("Unsupported extra ID3 frame: " + field);
                title |= field == "TIT2";
                duration |= field == "TLEN";
                artist |= field == "TPE1";
                album |= field == "TALB";
                offset += 10 + size;
            }
            if (!title || duration != withDuration || artist != tagsEnabled || album != tagsEnabled)
                throw new Exception("Tag selection or duration frames are incorrect.");
        }

        public static int Main()
        {
            string root = Path.Combine(Path.GetTempPath(), "ast-id3-v4-" + Guid.NewGuid().ToString("N"));
            Directory.CreateDirectory(root);
            try
            {
                string descriptor = Path.Combine(root, "entry-00000017.ast");
                byte[] manifest = new UTF8Encoding(false).GetBytes(
                    "ARCHIVE_STREAM_NATIVE_TRACK_V1\n" +
                    @"X:\Fixture\Elementis.zip.archive_stream|17|02 - Whitebear - Fever Dreams.mp3" + "\n");
                File.WriteAllBytes(descriptor, manifest);

                string withTagsAndDuration = VersionedMusicBeeAlias(descriptor, true, null, true);
                if (!AliasHasMetadataMarker(withTagsAndDuration, true, true))
                    throw new Exception("Expected V4 length alias directory with valid ID3 frames.");
                VerifyFrameSet(withTagsAndDuration, true, true);

                string noTags = VersionedMusicBeeAlias(descriptor, false, null, true);
                if (String.Equals(withTagsAndDuration, noTags, StringComparison.OrdinalIgnoreCase) ||
                    !AliasHasMetadataMarker(noTags, false, true))
                    throw new Exception("Title-preset versioning is not isolated.");
                VerifyFrameSet(noTags, false, true);

                string noDuration = VersionedMusicBeeAlias(descriptor, true, null, false);
                if (String.Equals(withTagsAndDuration, noDuration, StringComparison.OrdinalIgnoreCase) ||
                    !AliasHasMetadataMarker(noDuration, true, false))
                    throw new Exception("Unknown and known duration aliases must be distinct.");
                VerifyFrameSet(noDuration, true, false);

                string repeat = VersionedMusicBeeAlias(descriptor, true, null, true);
                if (!String.Equals(withTagsAndDuration, repeat, StringComparison.OrdinalIgnoreCase))
                    throw new Exception("Versioned paths must be idempotent.");

                byte[] corrupt = File.ReadAllBytes(withTagsAndDuration);
                corrupt[corrupt.Length - 1] ^= 1;
                File.WriteAllBytes(withTagsAndDuration, corrupt);
                if (AliasHasMetadataMarker(withTagsAndDuration, true, true))
                    throw new Exception("Corrupted descriptor trailer was accepted.");

                string repaired = VersionedMusicBeeAlias(descriptor, true, null, true);
                if (!String.Equals(withTagsAndDuration, repaired, StringComparison.OrdinalIgnoreCase) ||
                    !AliasHasMetadataMarker(repaired, true, true))
                    throw new Exception("Corrupted alias was not safely rebuilt.");
                VerifyFrameSet(repaired, true, true);

                if (!SameBytes(File.ReadAllBytes(descriptor), manifest))
                    throw new Exception("The real .ast manifest was changed.");

                Console.WriteLine("MUSICBEE_ID3_V4_TEST_PASS tags=1 duration=1 no_internal_frames=1 " +
                    "versioned_modes=3 idempotent=1 corrupt_alias_repaired=1 manifest_unchanged=1");
                return 0;
            }
            finally
            {
                Directory.Delete(root, true);
            }
        }
    }
}
