// Archive Stream Player / MusicBee.
// Read a single, genuine Info/Xing MP3 frame from a directly streamable
// archive entry after PCM duration has been independently verified.
// The frame is only a native playlist metadata hint. Playback always uses
// the paired .ast descriptor and the normal BASS archive decoder.
using System;
using System.Runtime.InteropServices;

namespace MusicBeePlugin
{
    public partial class Plugin
    {
        [DllImport("bass_archive_stream.dll", EntryPoint = "ArchiveStreamReadNativeMp3InfoFrameW",
            CharSet = CharSet.Unicode, CallingConvention = CallingConvention.StdCall)]
        private static extern int ReadNativeMp3InfoFrame(string virtualTrack,
            [Out] byte[] output, uint capacity, uint verifiedDurationMs);

        private static bool ValidateNativeMp3InfoFrame(byte[] data, int start, int length)
        {
            if (data == null || start < 0 || length < 48 || length > 4096 ||
                data.Length - start < length || data[start] != 0xff ||
                (data[start + 1] & 0xe0) != 0xe0)
                return false;
            int version = (data[start + 1] >> 3) & 3;
            int layer = (data[start + 1] >> 1) & 3;
            int bitrateIndex = (data[start + 2] >> 4) & 15;
            int sampleRateIndex = (data[start + 2] >> 2) & 3;
            if (version == 1 || layer != 1 || bitrateIndex == 0 ||
                bitrateIndex == 15 || sampleRateIndex == 3)
                return false;
            int[] rates = version == 3 ? new[] { 44100, 48000, 32000 } :
                version == 2 ? new[] { 22050, 24000, 16000 } :
                new[] { 11025, 12000, 8000 };
            int[] bitrates = version == 3 ?
                new[] { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } :
                new[] { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 };
            int expectedLength = (version == 3 ? 144 : 72) * bitrates[bitrateIndex] *
                1000 / rates[sampleRateIndex] + ((data[start + 2] >> 1) & 1);
            if (expectedLength != length)
                return false;

            bool mono = ((data[start + 3] >> 6) & 3) == 3;
            int sideInfo = version == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
            int crc = (data[start + 1] & 1) == 0 ? 2 : 0;
            int offset = start + 4 + crc + sideInfo;
            if (offset + 12 > start + length)
                return false;
            bool info = data[offset] == 'I' && data[offset + 1] == 'n' &&
                data[offset + 2] == 'f' && data[offset + 3] == 'o';
            bool xing = data[offset] == 'X' && data[offset + 1] == 'i' &&
                data[offset + 2] == 'n' && data[offset + 3] == 'g';
            if (!info && !xing)
                return false;
            uint flags = ((uint)data[offset + 4] << 24) | ((uint)data[offset + 5] << 16) |
                ((uint)data[offset + 6] << 8) | data[offset + 7];
            uint frames = ((uint)data[offset + 8] << 24) | ((uint)data[offset + 9] << 16) |
                ((uint)data[offset + 10] << 8) | data[offset + 11];
            return (flags & 1) != 0 && frames >= 2 && frames <= 10000000;
        }

        private static byte[] ReadValidatedArchiveMp3InfoFrame(string virtualTrack, long durationMs)
        {
            if (String.IsNullOrWhiteSpace(virtualTrack) || durationMs < 1000 ||
                durationMs > 604800000)
                return null;
            byte[] buffer = new byte[4096];
            try
            {
                int size = ReadNativeMp3InfoFrame(virtualTrack, buffer, (uint)buffer.Length,
                    (uint)durationMs);
                if (size <= 0 || !ValidateNativeMp3InfoFrame(buffer, 0, size))
                    return null;
                byte[] frame = new byte[size];
                Buffer.BlockCopy(buffer, 0, frame, 0, size);
                return frame;
            }
            catch (DllNotFoundException) { return null; }
            catch (EntryPointNotFoundException) { return null; }
            catch (BadImageFormatException) { return null; }
        }
    }
}
