#pragma once

// Shared, read-only metadata probe for player adapters. Only direct-streamable,
// unencrypted/non-solid archive entries are inspected, capped at 256 KiB.
// This is intentionally not a decompression fallback: solid/password entries
// must never trigger full archive extraction merely to display file tags.
// The common ArchiveTrackTagParser handles ID3v2, FLAC and Ogg/Vorbis/Opus.
#include "archive_adapter_preferences.h"
#include "archive_track_tags.h"
#include "../archive_backend/archive_backend.h"
#include "../archive_backend/archive_reader.h"
#include "../archive_backend/archive_stream_owner.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

inline bool ArchiveAdapterProbeEntryTags(const std::wstring& archive, const ArchiveEntry& entry, ArchiveTrackTags& output)
{
    output = ArchiveTrackTags{};
    if (archive.empty() || entry.isFolder || entry.isEncrypted || entry.isSolid ||
        !entry.isSupportedAudio || !entry.size)
        return false;

    const size_t slash = entry.path.find_last_of("/\\");
    const size_t dot = entry.path.find_last_of('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return false;
    std::string format = entry.path.substr(dot + 1);
    for (char& c : format)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (format != "mp3" && format != "flac" && format != "ogg" && format != "opus")
        return false;

    const std::string narrow = ArchiveAdapterWideToUtf8(archive);
    if (narrow.empty() || ArchiveGetEntryStreamCapability(narrow.c_str(), entry.index) !=
        ARCHIVE_STREAM_CAPABILITY_SUPPORTED)
        return false;

    ArchiveStreamOwner stream;
    std::string error;
    if (!stream.Open(archive.c_str(), entry.index, error))
        return false;

    constexpr size_t maximumPrefix = 256 * 1024;
    const size_t maximum = static_cast<size_t>((std::min)(entry.size, static_cast<uint64_t>(maximumPrefix)));
    if (maximum == 0)
        return false;
    std::vector<unsigned char> prefix(maximum);
    size_t received = 0;
    while (received < maximum)
    {
        const ArchiveStreamReadResult read = stream.ReadDetailed(prefix.data() + received, maximum - received);
        received += read.bytesRead;
        if (read.errorKind != ArchiveStreamErrorKind::None || !read.bytesRead)
            break;
    }
    stream.Close();
    return ArchiveTrackTagParser::Parse(format, prefix.data(), received, output) && output.Any();
}
