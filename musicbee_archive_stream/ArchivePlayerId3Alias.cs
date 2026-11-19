// MusicBee-compatible, metadata-only ID3v2.3 aliases for archive entries.
// The real audio remains in the archive, and the paired .ast reference remains
// plain text. The native BASS addon verifies the exact appended manifest bytes
// before accepting this alias. Ordinary MP3 files cannot match that contract.
using System;
using System.IO;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Security.Cryptography;
using Microsoft.Win32;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamSupportsMusicBeeTagAlias",
            CallingConvention = CallingConvention.StdCall)]
        private static extern int NativeMusicBeeTagAliasVersion();

        [DllImport("kernel32.dll", EntryPoint = "MoveFileExW",
            CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool MoveNativeFile(string existingPath, string newPath, uint flags);

        private const uint ReplaceExistingFile = 0x1;
        private const uint FlushRenamedFile = 0x8;

        private static bool NativeSupportsId3Alias()
        {
            try
            {
                return NativeMusicBeeTagAliasVersion() >= 2;
            }
            catch (EntryPointNotFoundException) { return false; }
            catch (DllNotFoundException) { return false; }
            catch (BadImageFormatException) { return false; }
        }

        private static void WriteId3Frame(Stream stream, string frameName, string value)
        {
            if (String.IsNullOrWhiteSpace(value) || frameName.Length != 4)
                return;

            string trimmed = value.Trim();
            if (trimmed.Length > 1024)
                trimmed = trimmed.Substring(0, 1024);
            byte[] bytes = Encoding.Unicode.GetBytes(trimmed);
            int length = bytes.Length + 3;
            byte[] header = Encoding.ASCII.GetBytes(frameName);
            stream.Write(header, 0, header.Length);
            // ID3v2.3 uses a big-endian frame size, unlike v2.4.
            stream.WriteByte((byte)((length >> 24) & 0xff));
            stream.WriteByte((byte)((length >> 16) & 0xff));
            stream.WriteByte((byte)((length >> 8) & 0xff));
            stream.WriteByte((byte)(length & 0xff));
            stream.WriteByte(0);
            stream.WriteByte(0);
            stream.WriteByte(1); // UTF-16 with BOM, valid ID3v2.3
            stream.WriteByte(0xff);
            stream.WriteByte(0xfe);
            stream.Write(bytes, 0, bytes.Length);
        }

        private static string MetadataMarker(bool useTags, bool hasDuration = false, bool hasMp3InfoFrame = false)
        {
            string template = "";
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
            {
                if (key != null)
                    template = key.GetValue("TitleTemplate", "<filename>") as string ?? "<filename>";
            }
            // When embedded tags are enabled, changing title presets does
            // not affect these entries. The marker supports lazy migration.
            return "ARCHIVE_STREAM_MUSICBEE_ID3_V5" +
                (hasMp3InfoFrame ? "_INFO" : "") + (hasDuration ? "_LENGTH|" : "|") +
                (useTags ? "TAGS" : "PRESET:" + template);
        }

        // BASS does not consistently forward metadata-only .mp3 aliases to
        // our add-on when the ID3 header contains non-audio marker frames
        // (PRIV, TXXX, COMM or extra text frames). Only normal music tags
        // belong in the alias. The settings identity lives in the versioned
        // directory name, never in an ID3 frame or the archive payload.
        private static string MetadataAliasDirectoryName(bool useTags, bool hasDuration, bool hasMp3InfoFrame = false)
        {
            using (SHA256 sha = SHA256.Create())
            {
                byte[] hash = sha.ComputeHash(Encoding.UTF8.GetBytes(MetadataMarker(useTags, hasDuration, hasMp3InfoFrame)));
                return "id3-" + BitConverter.ToString(hash, 0, 5).Replace("-", "").ToLowerInvariant();
            }
        }

        private static bool IsValidatedPlainId3Alias(string alias)
        {
            try
            {
                if (!IsId3Alias(alias))
                    return false;
                string descriptor = Path.ChangeExtension(alias, ".ast");
                if (!File.Exists(descriptor))
                    return false;
                byte[] bytes = File.ReadAllBytes(alias);
                byte[] manifest = File.ReadAllBytes(descriptor);
                if (bytes.Length < 48 || manifest.Length < 36 || manifest.Length > 16384 ||
                    bytes[3] != 3 || bytes[4] != 0 || bytes[5] != 0)
                    return false;
                for (int i = 6; i < 10; ++i)
                {
                    if ((bytes[i] & 0x80) != 0)
                        return false;
                }
                int length = 10 + (bytes[6] << 21) + (bytes[7] << 14) + (bytes[8] << 7) + bytes[9];
                int previewLength = bytes.Length - length - manifest.Length;
                if (length < 24 || length > 16384 || previewLength < 0 ||
                    previewLength > 4096)
                    return false;
                if (previewLength > 0 &&
                    !ValidateNativeMp3InfoFrame(bytes, length, previewLength))
                    return false;
                for (int i = 0; i < manifest.Length; ++i)
                {
                    if (bytes[length + previewLength + i] != manifest[i])
                        return false;
                }

                bool hasTitle = false;
                int pos = 10;
                while (pos + 10 <= length)
                {
                    string frame = Encoding.ASCII.GetString(bytes, pos, 4);
                    // Only the known, ordinary song fields used below. Extra
                    // ID3 frames can accidentally shadow our playback add-on.
                    if (frame != "TIT2" && frame != "TLEN" && frame != "TPE1" &&
                        frame != "TALB" && frame != "TPE2" && frame != "TRCK" &&
                        frame != "TCON" && frame != "TYER")
                        return false;
                    if (frame == "TIT2")
                        hasTitle = true;
                    int size = (bytes[pos + 4] << 24) | (bytes[pos + 5] << 16) |
                        (bytes[pos + 6] << 8) | bytes[pos + 7];
                    if (size < 2 || pos + 10 + size > length ||
                        bytes[pos + 8] != 0 || bytes[pos + 9] != 0)
                        return false;
                    pos += 10 + size;
                }
                return hasTitle && pos == length;
            }
            catch (IOException) { return false; }
            catch (UnauthorizedAccessException) { return false; }
        }

        private static bool AliasHasMetadataMarker(string alias, bool useTags, bool hasDuration = false, bool hasMp3InfoFrame = false)
        {
            string directory = Path.GetDirectoryName(alias);
            return !String.IsNullOrWhiteSpace(directory) &&
                String.Equals(Path.GetFileName(directory), MetadataAliasDirectoryName(useTags, hasDuration, hasMp3InfoFrame),
                    StringComparison.OrdinalIgnoreCase) &&
                IsValidatedPlainId3Alias(alias);
        }

        private static void CreateId3ManifestAlias(string descriptor, string alias, bool useTags,
            long durationMs = 0, byte[] mp3InfoFrame = null)
        {
            byte[] original = File.ReadAllBytes(descriptor);
            if (original.Length < 36 || original.Length > 16384)
                throw new IOException("Invalid native archive descriptor size.");
            string text = Encoding.UTF8.GetString(original);
            if (!text.StartsWith("ARCHIVE_STREAM_NATIVE_TRACK_V1\n", StringComparison.Ordinal))
                throw new IOException("Invalid native archive descriptor marker.");

            ArchiveEntryMetadata metadata = ReadEntryMetadata(Path.ChangeExtension(descriptor, ".mp3"), useTags);
            string fallback = Path.GetFileNameWithoutExtension(descriptor);
            try
            {
                string[] lines = text.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
                if (lines.Length >= 2)
                {
                    var result = new StringBuilder(2048);
                    if (FormatNativeTitle(lines[1].TrimEnd('\r'), result, (uint)result.Capacity) == 1 &&
                        result.Length > 0)
                        fallback = result.ToString();
                }
            }
            catch (Exception error)
            {
                Trace("NATIVE_ID3_TITLE_FALLBACK reason=" + error.GetType().Name);
            }
            string title = useTags && metadata.hasEmbeddedTitle ? metadata.title : fallback;
            if (String.IsNullOrWhiteSpace(title))
                title = Path.GetFileNameWithoutExtension(descriptor);
            if (String.IsNullOrWhiteSpace(title))
                throw new IOException("The archive track requires a nonempty title.");

            using (var tagFrames = new MemoryStream())
            {
                WriteId3Frame(tagFrames, "TIT2", title);
                // ID3v2.3 TLEN is milliseconds. MusicBee may use it as
                // initial duration metadata; unlike tag display preferences,
                // this is always written when a verified decoder length exists.
                if (durationMs > 0)
                    WriteId3Frame(tagFrames, "TLEN",
                        durationMs.ToString(System.Globalization.CultureInfo.InvariantCulture));
                if (useTags)
                {
                    WriteId3Frame(tagFrames, "TPE1", metadata.artist);
                    WriteId3Frame(tagFrames, "TALB", metadata.album);
                    WriteId3Frame(tagFrames, "TPE2", metadata.albumArtist);
                    WriteId3Frame(tagFrames, "TRCK", metadata.number == "0" ? null : metadata.number);
                    WriteId3Frame(tagFrames, "TCON", metadata.genre);
                    string year = metadata.year;
                    if (!String.IsNullOrEmpty(year) && year.Length >= 4)
                        year = year.Substring(0, 4);
                    WriteId3Frame(tagFrames, "TYER", year);
                }
                int length = checked((int)tagFrames.Length);
                if (length < 13 || length > 16384)
                    throw new IOException("The ID3 metadata exceeds the safe size limit.");
                using (var output = new FileStream(alias, FileMode.CreateNew, FileAccess.Write, FileShare.None))
                {
                    output.WriteByte((byte)'I'); output.WriteByte((byte)'D'); output.WriteByte((byte)'3');
                    output.WriteByte(3); output.WriteByte(0); output.WriteByte(0);
                    output.WriteByte((byte)((length >> 21) & 0x7f));
                    output.WriteByte((byte)((length >> 14) & 0x7f));
                    output.WriteByte((byte)((length >> 7) & 0x7f));
                    output.WriteByte((byte)(length & 0x7f));
                    tagFrames.Position = 0;
                    tagFrames.CopyTo(output);
                    // A single real MPEG Info/Xing frame, verified against
                    // decoded PCM duration, lets MusicBee's own file scanner
                    // populate the native Time column after playback begins.
                    // Never insert fabricated frames or audio payloads.
                    if (mp3InfoFrame != null && mp3InfoFrame.Length > 0)
                        output.Write(mp3InfoFrame, 0, mp3InfoFrame.Length);
                    output.Write(original, 0, original.Length);
                    output.Flush();
                }
            }
        }

        private static bool IsId3Alias(string path)
        {
            if (!File.Exists(path) || new FileInfo(path).Length > 32768)
                return false;
            using (var file = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                return file.ReadByte() == (byte)'I' && file.ReadByte() == (byte)'D' && file.ReadByte() == (byte)'3';
            }
        }

        // One stable URL per archive-entry index and metadata preference set.
        // A NEW path forces MusicBee to read ID3 tags rather than continue to
        // show cached metadata for the old .mp3 hard-link URL. The old path and
        // descriptor stay available for saved queues and ongoing playback.
        private static string VersionedMusicBeeAlias(string descriptor, bool useTags,
            string prebuiltAlias = null, bool includeDuration = false)
        {
            byte[] source = File.ReadAllBytes(descriptor);
            if (source.Length < 36 || source.Length > 16384)
                throw new IOException("The source native descriptor is invalid.");
            string text = Encoding.UTF8.GetString(source);
            string[] lines = text.Split(new[] { '\n' }, StringSplitOptions.RemoveEmptyEntries);
            if (lines.Length < 2 || !lines[0].Equals("ARCHIVE_STREAM_NATIVE_TRACK_V1", StringComparison.Ordinal))
                throw new IOException("The source native descriptor has no validated manifest.");
            string[] parts = lines[1].TrimEnd('\r').Split(new[] { '|' }, 3);
            uint index;
            if (parts.Length < 3 || !UInt32.TryParse(parts[1], out index))
                throw new IOException("The source archive entry index is invalid.");

            string root = Path.GetDirectoryName(descriptor);
            string leaf = Path.GetFileName(root);
            if (leaf.StartsWith("id3-", StringComparison.OrdinalIgnoreCase))
                root = Path.GetDirectoryName(root);
            // Duration probes are ONLY done by the low-priority metadata
            // worker, never while the first playlist is being created.
            // The initial V5 alias displays tags immediately. The background
            // verification worker publishes a V5_INFO_LENGTH version only
            // after validating a genuine Info/Xing frame against decoded PCM.
            string durationSource = prebuiltAlias ?? Path.ChangeExtension(descriptor, ".mp3");
            long durationMs = QueryArchiveDurationMs(durationSource, includeDuration);
            bool hasDuration = durationMs > 0;
            byte[] mp3InfoFrame = hasDuration ?
                ReadValidatedArchiveMp3InfoFrame(lines[1].TrimEnd('\r'), durationMs) : null;
            bool hasMp3InfoFrame = mp3InfoFrame != null;
            {
                // Hash the short preference/version identifier ONLY. We never
                // compute archive/audio CRCs or audio content hashes.
                string folder = Path.Combine(root,
                    MetadataAliasDirectoryName(useTags, hasDuration, hasMp3InfoFrame));
                Directory.CreateDirectory(folder);
                string baseName = "entry-" + index.ToString("D8", System.Globalization.CultureInfo.InvariantCulture);
                string newDescriptor = Path.Combine(folder, baseName + ".ast");
                string newAlias = Path.Combine(folder, baseName + ".mp3");
                lock (persistentTrackLock)
                {
                    if (!File.Exists(newDescriptor))
                        File.Copy(descriptor, newDescriptor, false);
                    else if (!System.Linq.Enumerable.SequenceEqual(
                        File.ReadAllBytes(newDescriptor), source))
                        throw new IOException("A versioned alias descriptor has conflicting archive identity.");

                    if (!AliasHasMetadataMarker(newAlias, useTags, hasDuration, hasMp3InfoFrame))
                    {
                        string pending = Path.Combine(folder, "_id3-" +
                            Guid.NewGuid().ToString("N").Substring(0, 12) + ".tmp");
                        try
                        {
                            if (!hasDuration && prebuiltAlias != null &&
                                IsValidatedPlainId3Alias(prebuiltAlias))
                                File.Copy(prebuiltAlias, pending, false);
                            else
                                CreateId3ManifestAlias(newDescriptor, pending, useTags, durationMs, mp3InfoFrame);
                            PersistMusicBeeAlias(pending, newAlias);
                        }
                        finally
                        {
                            if (File.Exists(pending))
                                File.Delete(pending);
                        }
                    }
                    if (!AliasHasMetadataMarker(newAlias, useTags, hasDuration, hasMp3InfoFrame))
                        throw new IOException("Unable to publish a versioned ID3 archive alias.");
                }
                return newAlias;
            }
        }

        // Persistent playlist paths never change. On first import, store the
        // ID3 file itself; if an older hard-link alias already exists, replace
        // only that link atomically. Never edit the shared .ast descriptor bytes.
        private static void PersistMusicBeeAlias(string temporaryAlias, string persistentAlias)
        {
            if (!IsId3Alias(temporaryAlias))
            {
                if (!File.Exists(persistentAlias) &&
                    !CreateNativeDescriptorHardLink(persistentAlias, Path.ChangeExtension(persistentAlias, ".ast"), IntPtr.Zero))
                    throw new IOException("Unable to preserve the legacy native archive alias.");
                return;
            }

            if (!File.Exists(persistentAlias))
            {
                File.Copy(temporaryAlias, persistentAlias, false);
                return;
            }

            byte[] oldBytes = File.ReadAllBytes(persistentAlias);
            byte[] nextBytes = File.ReadAllBytes(temporaryAlias);
            if (oldBytes.Length == nextBytes.Length &&
                System.Linq.Enumerable.SequenceEqual(oldBytes, nextBytes))
                return;

            string pending = persistentAlias + ".id3-" + Guid.NewGuid().ToString("N").Substring(0, 10);
            try
            {
                File.WriteAllBytes(pending, nextBytes);
                if (!MoveNativeFile(pending, persistentAlias, ReplaceExistingFile | FlushRenamedFile))
                {
                    // If playback currently holds the alias open, retain the
                    // previously playable reference rather than interrupt it.
                    Trace("NATIVE_ID3_ALIAS_REPLACE_DEFERRED win32=" +
                        Marshal.GetLastWin32Error().ToString(System.Globalization.CultureInfo.InvariantCulture));
                }
            }
            finally
            {
                if (File.Exists(pending))
                    File.Delete(pending);
            }
        }
    }
}
