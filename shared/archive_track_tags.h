#pragma once

// Read-only, bounded tag parser shared by archive-backed player adapters.
// Accepts an already-read prefix of an audio entry; never extracts audio,
// generates CRCs, or changes the archive. Supports ID3v2.2/2.3/2.4,
// FLAC Vorbis comments, and Ogg Vorbis/Opus comments.
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

struct ArchiveTrackTags
{
    std::string title, artist, album, albumArtist, genre, year;
    unsigned trackNumber = 0;
    bool Any() const { return !title.empty() || !artist.empty() || !album.empty() || !albumArtist.empty() || !genre.empty() || !year.empty() || trackNumber != 0; }
};

namespace ArchiveTrackTagParser
{
    inline uint32_t BE32(const unsigned char* p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
    inline uint32_t LE32(const unsigned char* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
    inline uint32_t SyncSafe(const unsigned char* p) { return (uint32_t(p[0]) << 21) | (uint32_t(p[1]) << 14) | (uint32_t(p[2]) << 7) | p[3]; }

    inline std::string Clean(std::string value)
    {
        std::string result;
        result.reserve((std::min)(value.size(), size_t(1024)));
        for (size_t i = 0; i < value.size() && result.size() < 1024; ++i)
        {
            const unsigned char c = static_cast<unsigned char>(value[i]);
            if (c == 0) break;
            if (c == '\t' || c == '\r' || c == '\n') { result += ' '; continue; }
            if (c < 32 || c == 127) continue;
            result += static_cast<char>(c);
        }
        const auto first = result.find_first_not_of(' ');
        if (first == std::string::npos) return {};
        const auto last = result.find_last_not_of(' ');
        return result.substr(first, last - first + 1);
    }

    inline std::string UTF8(const std::wstring& value)
    {
        if (value.empty()) return {};
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return {};
        std::string output(size_t(size), '\0');
        return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), size, nullptr, nullptr) == size ? Clean(output) : std::string();
    }

    inline std::string DecodeText(const unsigned char* data, size_t size)
    {
        if (!size) return {};
        const unsigned encoding = data[0];
        ++data; --size;
        if (encoding == 3) return Clean(std::string(reinterpret_cast<const char*>(data), size));
        if (encoding == 0)
        {
            std::wstring wide;
            for (size_t i = 0; i < size && data[i]; ++i)
                wide += wchar_t(data[i]); // ID3v2 Latin-1, independent of system codepage
            return UTF8(wide);
        }
        if (encoding != 1 && encoding != 2) return {};
        bool little = false;
        if (encoding == 1)
        {
            if (size < 2) return {};
            if (data[0] == 0xff && data[1] == 0xfe) little = true;
            else if (data[0] == 0xfe && data[1] == 0xff) little = false;
            else return {}; // ID3v2.3 requires BOM for encoding 1
            data += 2; size -= 2;
        }
        std::wstring wide;
        for (size_t i = 0; i + 1 < size; i += 2)
        {
            const uint16_t c = little ? uint16_t(data[i] | (data[i + 1] << 8)) : uint16_t((data[i] << 8) | data[i + 1]);
            if (!c) break;
            wide += wchar_t(c);
        }
        return UTF8(wide);
    }

    inline unsigned TrackNumber(const std::string& value)
    {
        unsigned n = 0;
        for (const unsigned char c : value)
        {
            if (c < '0' || c > '9') break;
            n = n * 10 + (c - '0');
            if (n > 9999) return 0;
        }
        return n;
    }

    inline void Apply(ArchiveTrackTags& tags, std::string name, const std::string& raw)
    {
        for (char& c : name) c = char(std::toupper(static_cast<unsigned char>(c)));
        const std::string value = Clean(raw);
        if (value.empty()) return;
        if (name == "TIT2" || name == "TT2" || name == "TITLE") tags.title = value;
        else if (name == "TPE1" || name == "TP1" || name == "ARTIST") tags.artist = value;
        else if (name == "TALB" || name == "TAL" || name == "ALBUM") tags.album = value;
        else if (name == "TPE2" || name == "TP2" || name == "ALBUMARTIST" || name == "ALBUM ARTIST") tags.albumArtist = value;
        else if (name == "TCON" || name == "TCO" || name == "GENRE") tags.genre = value;
        else if (name == "TYER" || name == "TYE" || name == "TDRC" || name == "DATE" || name == "YEAR") tags.year = value;
        else if (name == "TRCK" || name == "TRK" || name == "TRACKNUMBER") tags.trackNumber = TrackNumber(value);
    }

    inline bool ParseID3(const unsigned char* data, size_t size, ArchiveTrackTags& tags)
    {
        if (size < 10 || std::memcmp(data, "ID3", 3) != 0 || data[3] < 2 || data[3] > 4) return false;
        const unsigned major = data[3];
        const unsigned flags = data[5];
        if (flags & 0x80) return false; // Global unsynchronisation requires a different frame decoder.
        if ((data[6] | data[7] | data[8] | data[9]) & 0x80) return false;
        size_t end = (std::min)(size, size_t(10) + SyncSafe(data + 6));
        size_t at = 10;
        if (flags & 0x40)
        {
            if (major == 2 || at + 4 > end) return false;
            const uint32_t extra = major == 4 ? SyncSafe(data + at) : BE32(data + at) + 4;
            if (extra < 4 || extra > end - at) return false;
            at += extra;
        }
        while (at + (major == 2 ? 6 : 10) <= end)
        {
            const size_t header = major == 2 ? 6 : 10;
            if (data[at] == 0) break;
            bool valid = true;
            for (size_t i = 0; i < (major == 2 ? 3u : 4u); ++i)
                if (!(data[at + i] >= 'A' && data[at + i] <= 'Z') && !(data[at + i] >= '0' && data[at + i] <= '9')) valid = false;
            if (!valid) break;
            const std::string name(reinterpret_cast<const char*>(data + at), major == 2 ? 3 : 4);
            const uint32_t bytes = major == 2 ? (uint32_t(data[at + 3]) << 16) | (uint32_t(data[at + 4]) << 8) | data[at + 5]
                : major == 4 ? SyncSafe(data + at + 4) : BE32(data + at + 4);
            const bool unsupportedFlags = major == 3 ? (data[at + 9] & 0xc0) != 0
                : major == 4 ? (data[at + 9] & 0x0f) != 0 : false;
            at += header;
            if (bytes > end - at) break;
            if (!unsupportedFlags && name[0] == 'T' && bytes > 1)
                Apply(tags, name, DecodeText(data + at, bytes));
            at += bytes;
        }
        return tags.Any();
    }

    inline bool ParseVorbisComments(const unsigned char* data, size_t size, ArchiveTrackTags& tags)
    {
        if (size < 8) return false;
        const size_t vendor = LE32(data);
        if (vendor > size - 8) return false;
        size_t at = 4 + vendor;
        if (at + 4 > size) return false;
        const uint32_t count = LE32(data + at);
        at += 4;
        if (count > 10000) return false;
        for (uint32_t i = 0; i < count; ++i)
        {
            if (at + 4 > size) break;
            const size_t bytes = LE32(data + at);
            at += 4;
            if (bytes > size - at) break;
            if (bytes && bytes <= 4096)
            {
                const std::string item(reinterpret_cast<const char*>(data + at), bytes);
                const auto equal = item.find('=');
                if (equal != std::string::npos && equal > 0 && equal < 64)
                    Apply(tags, item.substr(0, equal), item.substr(equal + 1));
            }
            at += bytes;
        }
        return tags.Any();
    }

    inline bool ParseFLAC(const unsigned char* data, size_t size, ArchiveTrackTags& tags)
    {
        if (size < 8 || std::memcmp(data, "fLaC", 4) != 0) return false;
        size_t at = 4;
        while (at + 4 <= size)
        {
            const bool last = (data[at] & 0x80) != 0;
            const unsigned type = data[at] & 0x7f;
            const size_t bytes = (size_t(data[at + 1]) << 16) | (size_t(data[at + 2]) << 8) | data[at + 3];
            at += 4;
            if (bytes > size - at) break;
            if (type == 4 && ParseVorbisComments(data + at, bytes, tags)) return true;
            at += bytes;
            if (last) break;
        }
        return tags.Any();
    }

    inline bool ParseOgg(const unsigned char* data, size_t size, ArchiveTrackTags& tags)
    {
        if (size < 27 || std::memcmp(data, "OggS", 4) != 0) return false;
        size_t at = 0;
        std::vector<unsigned char> packet;
        while (at + 27 <= size)
        {
            if (std::memcmp(data + at, "OggS", 4) != 0 || data[at + 4] != 0) break;
            const size_t segments = data[at + 26];
            const size_t lacing = at + 27;
            if (segments > size - lacing) break;
            size_t payload = lacing + segments;
            for (size_t i = 0; i < segments; ++i)
            {
                const size_t bytes = data[lacing + i];
                if (bytes > size - payload) return tags.Any();
                if (packet.size() + bytes > 256 * 1024) return tags.Any();
                packet.insert(packet.end(), data + payload, data + payload + bytes);
                payload += bytes;
                if (bytes < 255)
                {
                    if (packet.size() > 7 && packet[0] == 3 && std::memcmp(packet.data() + 1, "vorbis", 6) == 0)
                        return ParseVorbisComments(packet.data() + 7, packet.size() - 7, tags);
                    if (packet.size() > 8 && std::memcmp(packet.data(), "OpusTags", 8) == 0)
                        return ParseVorbisComments(packet.data() + 8, packet.size() - 8, tags);
                    packet.clear();
                }
            }
            at = payload;
        }
        return tags.Any();
    }

    inline bool Parse(const std::string& extension, const unsigned char* data, size_t size, ArchiveTrackTags& tags)
    {
        if (!data) return false;
        if (extension == "mp3") return ParseID3(data, size, tags);
        if (extension == "flac") return ParseFLAC(data, size, tags);
        if (extension == "ogg" || extension == "opus") return ParseOgg(data, size, tags);
        return false;
    }

    inline std::string Escape(const std::string& input)
    {
        constexpr char hex[] = "0123456789ABCDEF";
        std::string out;
        out.reserve(input.size() * 3);
        for (const unsigned char c : input)
        {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        return out;
    }
}
