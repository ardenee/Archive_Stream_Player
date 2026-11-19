#pragma once

// Lightweight, streaming-first audio-property probe for host playlist metadata.
// Reads a bounded prefix (including ID3 artwork) and stops at the first genuine
// MPEG Info/Xing frame, FLAC STREAMINFO, or RIFF/WAV data header. No full-track
// decode, archive extraction, content CRC calculation, or disk cache writes.
// Untrusted/unsupported headers simply leave properties unknown.
#include "../archive_backend/archive_backend.h"
#include "../archive_backend/archive_reader.h"
#include "../archive_backend/archive_stream_owner.h"
#include "archive_adapter_preferences.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct ArchiveAdapterAudioInfo
{
    double durationSeconds = 0.0;
    unsigned sampleRate = 0;
    unsigned bitrateKbps = 0;
    unsigned channels = 0;
    bool HasDuration() const { return durationSeconds >= 1.0 && durationSeconds <= 604800.0; }
    double DurationMilliseconds() const { return durationSeconds * 1000.0; }
};

namespace ArchiveAdapterAudioProbe
{
    inline uint32_t BE32(const unsigned char* p)
    {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    inline uint32_t LE32(const unsigned char* p)
    {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    inline bool ReadExact(ArchiveStreamOwner& stream, unsigned char* bytes, size_t wanted)
    {
        size_t got = 0;
        while (got < wanted)
        {
            auto result = stream.ReadDetailed(bytes + got, wanted - got);
            if (result.errorKind != ArchiveStreamErrorKind::None || !result.bytesRead)
                return false;
            got += result.bytesRead;
        }
        return true;
    }

    inline bool Discard(ArchiveStreamOwner& stream, size_t count)
    {
        unsigned char block[32768]{};
        while (count)
        {
            const size_t requested = (std::min)(count, sizeof(block));
            if (!ReadExact(stream, block, requested))
                return false;
            count -= requested;
        }
        return true;
    }

    inline bool ParseMp3InfoFrame(const unsigned char* bytes, size_t size, ArchiveAdapterAudioInfo& out)
    {
        if (!bytes || size < 48 || bytes[0] != 0xff || (bytes[1] & 0xe0) != 0xe0)
            return false;
        const unsigned version = (bytes[1] >> 3) & 3u;
        const unsigned layer = (bytes[1] >> 1) & 3u;
        const unsigned bitrateIndex = (bytes[2] >> 4) & 15u;
        const unsigned sampleIndex = (bytes[2] >> 2) & 3u;
        if (version == 1 || layer != 1 || bitrateIndex == 0 || bitrateIndex == 15 || sampleIndex == 3)
            return false;
        constexpr unsigned rates[3][3] = {{11025, 12000, 8000}, {22050, 24000, 16000}, {44100, 48000, 32000}};
        constexpr unsigned mpeg1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
        constexpr unsigned mpeg2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
        const unsigned rate = rates[version == 3 ? 2 : version == 2 ? 1 : 0][sampleIndex];
        const unsigned kbps = (version == 3 ? mpeg1 : mpeg2)[bitrateIndex];
        const size_t frameBytes = ((version == 3 ? 144u : 72u) * kbps * 1000u / rate) + ((bytes[2] >> 1) & 1u);
        if (frameBytes < 48 || frameBytes > size || frameBytes > 4096)
            return false;
        const unsigned channels = ((bytes[3] >> 6) & 3u) == 3 ? 1u : 2u;
        const size_t side = version == 3 ? (channels == 1 ? 17u : 32u) : (channels == 1 ? 9u : 17u);
        const size_t marker = 4u + ((bytes[1] & 1u) ? 0u : 2u) + side;
        if (marker + 12u > frameBytes ||
            (std::memcmp(bytes + marker, "Info", 4) != 0 &&
             std::memcmp(bytes + marker, "Xing", 4) != 0))
            return false;

        const uint32_t flags = BE32(bytes + marker + 4);
        const uint32_t frames = BE32(bytes + marker + 8);
        if (!(flags & 1u) || frames < 40 || frames > 10000000)
            return false;
        // Xing/Info frame counts include encoder priming and final padding.
        // AIMP's playback decoder applies the LAME gapless delay/padding, so
        // the playlist Time field must do the same to avoid one-second
        // rounding differences. Read only the genuine LAME extension inside
        // the first MPEG frame; never infer a delay when it is absent.
        const uint64_t samplesPerFrame = version == 3 ? 1152u : 576u;
        uint64_t playableSamples = uint64_t(frames) * samplesPerFrame;
        size_t encoderOffset = marker + 8u;
        if (flags & 1u)
            encoderOffset += 4u;
        if (flags & 2u)
            encoderOffset += 4u;
        if (flags & 4u)
            encoderOffset += 100u;
        if (flags & 8u)
            encoderOffset += 4u;
        if (encoderOffset + 24u <= frameBytes)
        {
            const unsigned char* encoder = bytes + encoderOffset;
            const bool lame = (encoder[0] == 'L' || encoder[0] == 'l') &&
                (encoder[1] == 'A' || encoder[1] == 'a') &&
                (encoder[2] == 'M' || encoder[2] == 'm') &&
                (encoder[3] == 'E' || encoder[3] == 'e');
            if (lame)
            {
                const unsigned delay = (unsigned(encoder[21]) << 4) | (encoder[22] >> 4);
                const unsigned padding = ((unsigned(encoder[22]) & 0x0fu) << 8) | encoder[23];
                if (uint64_t(delay) + padding < playableSamples)
                    playableSamples -= delay + padding;
            }
        }
        const double seconds = double(playableSamples) / double(rate);
        if (seconds < 1.0 || seconds > 604800.0)
            return false;

        unsigned averageKbps = kbps;
        if ((flags & 2u) && marker + 16u <= frameBytes)
        {
            const uint32_t compressedBytes = BE32(bytes + marker + 12);
            if (compressedBytes)
            {
                const double average = compressedBytes * 8.0 / seconds / 1000.0;
                if (average >= 8.0 && average <= 640.0)
                    averageKbps = static_cast<unsigned>(average + 0.5);
            }
        }
        out.durationSeconds = seconds;
        out.sampleRate = rate;
        out.bitrateKbps = averageKbps;
        out.channels = channels;
        return true;
    }

    inline bool ParseFlacStreamInfo(const unsigned char* bytes, size_t size, ArchiveAdapterAudioInfo& out)
    {
        if (!bytes || size < 42 || std::memcmp(bytes, "fLaC", 4) != 0 ||
            (bytes[4] & 0x7fu) != 0 || ((unsigned(bytes[5]) << 16) | (unsigned(bytes[6]) << 8) | bytes[7]) != 34)
            return false;
        const uint64_t packed = (uint64_t(BE32(bytes + 18)) << 32) | BE32(bytes + 22);
        const unsigned rate = unsigned((packed >> 44) & 0xfffffu);
        const unsigned channels = unsigned((packed >> 41) & 7u) + 1u;
        const uint64_t samples = packed & 0xfffffffffu;
        if (rate < 8000 || rate > 384000 || channels == 0 || channels > 8 || !samples)
            return false;
        const double seconds = double(samples) / rate;
        if (seconds < 1.0 || seconds > 604800.0)
            return false;
        out.durationSeconds = seconds;
        out.sampleRate = rate;
        out.channels = channels;
        return true;
    }

    inline bool ParseWaveHeader(const unsigned char* bytes, size_t size, ArchiveAdapterAudioInfo& out)
    {
        if (!bytes || size < 44 || std::memcmp(bytes, "RIFF", 4) != 0 || std::memcmp(bytes + 8, "WAVE", 4) != 0)
            return false;
        unsigned sampleRate = 0;
        unsigned channels = 0;
        uint32_t byteRate = 0;
        uint32_t dataBytes = 0;
        for (size_t at = 12; at + 8 <= size;)
        {
            const uint32_t chunkBytes = LE32(bytes + at + 4);
            const size_t body = at + 8;
            if (std::memcmp(bytes + at, "fmt ", 4) == 0 && chunkBytes >= 16 && body + 16 <= size)
            {
                const uint16_t codec = uint16_t(bytes[body]) | (uint16_t(bytes[body + 1]) << 8);
                if (codec != 1 && codec != 3 && codec != 0xfffe)
                    return false;
                channels = unsigned(bytes[body + 2]) | (unsigned(bytes[body + 3]) << 8);
                sampleRate = LE32(bytes + body + 4);
                byteRate = LE32(bytes + body + 8);
            }
            if (std::memcmp(bytes + at, "data", 4) == 0)
            {
                dataBytes = chunkBytes;
                break;
            }
            if (chunkBytes > size - body || at + 8 + chunkBytes + (chunkBytes & 1u) > size)
                break;
            at = body + chunkBytes + (chunkBytes & 1u);
        }
        if (!byteRate || !dataBytes || sampleRate < 8000 || sampleRate > 384000 || channels < 1 || channels > 8)
            return false;
        const double seconds = double(dataBytes) / byteRate;
        if (seconds < 1.0 || seconds > 604800.0)
            return false;
        out.durationSeconds = seconds;
        out.sampleRate = sampleRate;
        out.channels = channels;
        out.bitrateKbps = (byteRate * 8u) / 1000u;
        return true;
    }
}

inline bool ArchiveAdapterProbeEntryAudio(const std::wstring& archive, const ArchiveEntry& entry, ArchiveAdapterAudioInfo& output)
{
    output = ArchiveAdapterAudioInfo{};
    if (archive.empty() || !entry.isSupportedAudio || entry.isFolder || entry.isSolid || entry.isEncrypted || entry.size < 48)
        return false;
    const size_t dot = entry.path.find_last_of('.');
    if (dot == std::string::npos)
        return false;
    std::string ext = entry.path.substr(dot + 1);
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "mp3" && ext != "flac" && ext != "wav")
        return false;

    const std::string narrow = ArchiveAdapterWideToUtf8(archive);
    if (narrow.empty() ||
        ArchiveGetEntryStreamCapability(narrow.c_str(), entry.index) != ARCHIVE_STREAM_CAPABILITY_SUPPORTED)
        return false;
    ArchiveStreamOwner stream;
    std::string error;
    if (!stream.Open(archive.c_str(), entry.index, error))
        return false;

    unsigned char prefix[4096]{};
    size_t filled = 0;
    if (ext == "mp3")
    {
        if (!ArchiveAdapterAudioProbe::ReadExact(stream, prefix, 10))
            return false;
        filled = 10;
        if (std::memcmp(prefix, "ID3", 3) == 0)
        {
            if (prefix[6] & 0x80 || prefix[7] & 0x80 || prefix[8] & 0x80 || prefix[9] & 0x80)
                return false;
            const size_t tagBytes = 10u + (size_t(prefix[6]) << 21) + (size_t(prefix[7]) << 14) +
                (size_t(prefix[8]) << 7) + prefix[9] + ((prefix[5] & 0x10u) ? 10u : 0u);
            // User-facing metadata must not block on huge embedded artwork.
            if (tagBytes > 4u * 1024u * 1024u || tagBytes > entry.size)
                return false;
            if (!ArchiveAdapterAudioProbe::Discard(stream, tagBytes - 10u))
                return false;
            filled = 0;
        }
    }
    const uint64_t remain = entry.size > stream.Tell() ? entry.size - stream.Tell() : 0;
    const size_t toRead = static_cast<size_t>((std::min)(uint64_t(sizeof(prefix) - filled), remain));
    if (!toRead || !ArchiveAdapterAudioProbe::ReadExact(stream, prefix + filled, toRead))
        return false;
    filled += toRead;
    if (ext == "mp3")
    {
        // Scan only the first 1 KiB beyond the ID3 tag; allow complete
        // frames up to 4 KiB and reject missing/invalid Info/Xing fields.
        for (size_t at = 0; at < 1024 && at + 48 <= filled; ++at)
        {
            if (ArchiveAdapterAudioProbe::ParseMp3InfoFrame(prefix + at, filled - at, output))
                return true;
        }
        return false;
    }
    if (ext == "flac")
        return ArchiveAdapterAudioProbe::ParseFlacStreamInfo(prefix, filled, output);
    return ArchiveAdapterAudioProbe::ParseWaveHeader(prefix, filled, output);
}
