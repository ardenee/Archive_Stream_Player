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

        private static string MetadataMarker(bool useTags)
        {
            string template = "";
            using (RegistryKey key = Registry.CurrentUser.OpenSubKey(@"Software\ArchiveStreamPlayer\Adapters", false))
            {
                if (key != null)
                    template = key.GetValue("TitleTemplate", "<filename>") as string ?? "<filename>";
            }
            // When embedded tags are enabled, changing title presets does
            // not affect these entries. The marker supports lazy migration.
            return "ARCHIVE_STREAM_MUSICBEE_ID3_V2|" +
                (useTags ? "TAGS" : "PRESET:" + template);
        }

        private static void WriteId3OpaqueFrame(Stream stream, string frameName, byte[] payload)
        {
            if (payload == null || payload.Length == 0)
                return;
            byte[] name = Encoding.ASCII.GetBytes(frameName);
            stream.Write(name, 0, name.Length);
            stream.WriteByte((byte)((payload.Length >> 24) & 0xff));
            stream.WriteByte((byte)((payload.Length >> 16) & 0xff));
            stream.WriteByte((byte)((payload.Length >> 8) & 0xff));
            stream.WriteByte((byte)(payload.Length & 0xff));
            stream.WriteByte(0);
            stream.WriteByte(0);
            stream.Write(payload, 0, payload.Length);
        }

        private static bool AliasHasMetadataMarker(string alias, bool useTags)
        {
            if (!IsId3Alias(alias))
                return false;
            byte[] bytes = File.ReadAllBytes(alias);
            byte[] marker = Encoding.UTF8.GetBytes(MetadataMarker(useTags));
            for (int i = 0; i + marker.Length <= bytes.Length; ++i)
            {
                int j = 0;
                while (j < marker.Length && bytes[i + j] == marker[j])
                    ++j;
                if (j == marker.Length)
                    return true;
            }
            return false;
        }

        private static void CreateId3ManifestAlias(string descriptor, string alias, bool useTags)
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
                WriteId3OpaqueFrame(tagFrames, "PRIV", Encoding.UTF8.GetBytes(
                    "ArchiveStreamPlayer\0" + MetadataMarker(useTags)));
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
        private static string VersionedMusicBeeAlias(string descriptor, bool useTags, string prebuiltAlias = null)
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
            using (SHA256 sha = SHA256.Create())
            {
                // Hash only a short settings-mode identifier, NEVER audio or
                // archive content. Content CRCs remain solely archive headers.
                byte[] hash = sha.ComputeHash(Encoding.UTF8.GetBytes(MetadataMarker(useTags)));
                string version = BitConverter.ToString(hash, 0, 5).Replace("-", "").ToLowerInvariant();
                string folder = Path.Combine(root, "id3-" + version);
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

                    if (!AliasHasMetadataMarker(newAlias, useTags))
                    {
                        string pending = Path.Combine(folder, "_id3-" +
                            Guid.NewGuid().ToString("N").Substring(0, 12) + ".tmp");
                        try
                        {
                            if (prebuiltAlias != null && AliasHasMetadataMarker(prebuiltAlias, useTags))
                                File.Copy(prebuiltAlias, pending, false);
                            else
                                CreateId3ManifestAlias(newDescriptor, pending, useTags);
                            PersistMusicBeeAlias(pending, newAlias);
                        }
                        finally
                        {
                            if (File.Exists(pending))
                                File.Delete(pending);
                        }
                    }
                    if (!AliasHasMetadataMarker(newAlias, useTags))
                        throw new IOException("Unable to publish a versioned ID3 archive alias.");
                }
                return newAlias;
            }
        }

        // Upgrade a legacy .mp3 hard link (or a tag mode changed since the
        // last import) without changing the saved MusicBee playlist URL. This
        // runs off the host UI thread, and only rebuilds stale alias entries.
        private static bool RefreshPersistentMusicBeeId3Alias(string alias, bool useTags)
        {
            if (!NativeSupportsId3Alias() || !IsPersistentNativeLibraryTrack(alias))
                return false;
            string descriptor = Path.ChangeExtension(alias, ".ast");
            lock (persistentTrackLock)
            {
                if (AliasHasMetadataMarker(alias, useTags))
                    return false;
                string pending = Path.Combine(Path.GetDirectoryName(alias),
                    "_ast-id3-" + Guid.NewGuid().ToString("N").Substring(0, 12) + ".tmp");
                try
                {
                    CreateId3ManifestAlias(descriptor, pending, useTags);
                    PersistMusicBeeAlias(pending, alias);
                    return AliasHasMetadataMarker(alias, useTags);
                }
                catch (Exception error)
                {
                    Trace("NATIVE_ID3_ALIAS_UPGRADE_FAILED type=" + error.GetType().Name);
                    return false;
                }
                finally
                {
                    if (File.Exists(pending))
                        File.Delete(pending);
                }
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
