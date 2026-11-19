// Decode-length metadata for native archive aliases. This uses BASS's
// existing Archive Stream decoder and its stream/RAM/cache routing. It does
// not estimate time from compressed size, nor calculate archive content CRCs.
// Duration is independent of the Use File Tags display preference.
using System;
using System.Globalization;
using System.IO;
using System.Runtime.InteropServices;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        private const uint BassUnicodeFile = 0x80000000u;
        private const uint BassDecodeFile = 0x200000u;

        [DllImport("bass.dll", EntryPoint = "BASS_StreamCreateFile", CharSet = CharSet.Unicode,
            CallingConvention = CallingConvention.StdCall)]
        private static extern uint BassOpenArchiveAlias(int mem, string filename, ulong offset, ulong length, uint flags);
        [DllImport("bass.dll", EntryPoint = "BASS_ChannelGetLength", CallingConvention = CallingConvention.StdCall)]
        private static extern ulong BassGetArchivePcmLength(uint handle, uint mode);
        [DllImport("bass.dll", EntryPoint = "BASS_ChannelBytes2Seconds", CallingConvention = CallingConvention.StdCall)]
        private static extern double BassGetArchiveSeconds(uint handle, ulong bytes);
        [DllImport("bass.dll", EntryPoint = "BASS_StreamFree", CallingConvention = CallingConvention.StdCall)]
        [return: MarshalAs(UnmanagedType.Bool)]
        private static extern bool BassCloseArchiveAlias(uint handle);

        private static long QueryArchiveDurationMs(string alias)
        {
            if (String.IsNullOrWhiteSpace(alias) || !File.Exists(alias))
                return 0;
            string archive, member, virtualPath;
            if (!TryReadArchiveEntryPath(alias, out archive, out member, out virtualPath) ||
                !File.Exists(archive))
                return 0;

            string cache = "";
            long sourceSize = 0;
            long sourceTicks = 0;
            try
            {
                var file = new FileInfo(archive);
                sourceSize = file.Length;
                sourceTicks = file.LastWriteTimeUtc.Ticks;
                string parent = Path.GetDirectoryName(alias);
                if (Path.GetFileName(parent).StartsWith("id3-", StringComparison.OrdinalIgnoreCase))
                    parent = Path.GetDirectoryName(parent);
                int marker = virtualPath.IndexOf(".archive_stream|", StringComparison.OrdinalIgnoreCase);
                int indexEnd = virtualPath.IndexOf('|', marker + 16);
                string indexPart = marker < 0 || indexEnd <= marker
                    ? "" : virtualPath.Substring(marker + 16, indexEnd - marker - 16);
                uint index;
                if (UInt32.TryParse(indexPart, NumberStyles.None, CultureInfo.InvariantCulture, out index))
                {
                    cache = Path.Combine(parent, "duration-" + index.ToString("D8", CultureInfo.InvariantCulture) + ".txt");
                    if (File.Exists(cache))
                    {
                        string[] values = File.ReadAllText(cache).Split('|');
                        long size, ticks, duration;
                        if (values.Length == 3 &&
                            Int64.TryParse(values[0], out size) && size == sourceSize &&
                            Int64.TryParse(values[1], out ticks) && ticks == sourceTicks &&
                            Int64.TryParse(values[2], out duration) &&
                            duration > 0 && duration <= 7L * 24 * 60 * 60 * 1000)
                            return duration;
                    }
                }
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DURATION_CACHE_READ_FAIL type=" + error.GetType().Name);
            }

            uint stream = 0;
            try
            {
                // Decode from the paired .ast manifest, not the tiny ID3
                // label file: MusicBee/BASS can classify an ID3-only alias
                // as a malformed MP3 before trying our archive add-on.
                stream = BassOpenArchiveAlias(0, Path.ChangeExtension(alias, ".ast"),
                    0, 0, BassUnicodeFile | BassDecodeFile);
                if (stream == 0)
                {
                    Trace("ARCHIVE_DURATION_UNAVAILABLE reason=decoder_open");
                    return 0;
                }
                ulong length = BassGetArchivePcmLength(stream, 0);
                if (length == 0 || length == UInt64.MaxValue)
                    return 0;
                double seconds = BassGetArchiveSeconds(stream, length);
                if (Double.IsNaN(seconds) || Double.IsInfinity(seconds) || seconds <= 0 ||
                    seconds > 7d * 24 * 60 * 60)
                    return 0;
                long milliseconds = (long)Math.Round(seconds * 1000, MidpointRounding.AwayFromZero);
                if (milliseconds > 0 && cache.Length != 0)
                {
                    try
                    {
                        string contents = sourceSize.ToString(CultureInfo.InvariantCulture) + "|" +
                            sourceTicks.ToString(CultureInfo.InvariantCulture) + "|" +
                            milliseconds.ToString(CultureInfo.InvariantCulture);
                        string temp = cache + "." + Guid.NewGuid().ToString("N").Substring(0, 12) + ".tmp";
                        File.WriteAllText(temp, contents);
                        if (File.Exists(cache))
                            File.Delete(cache);
                        File.Move(temp, cache);
                    }
                    catch (Exception error)
                    {
                        Trace("ARCHIVE_DURATION_CACHE_WRITE_FAIL type=" + error.GetType().Name);
                    }
                }
                return milliseconds;
            }
            catch (Exception error)
            {
                Trace("ARCHIVE_DURATION_PROBE_FAIL type=" + error.GetType().Name);
                return 0;
            }
            finally
            {
                if (stream != 0)
                    BassCloseArchiveAlias(stream);
            }
        }
    }
}
