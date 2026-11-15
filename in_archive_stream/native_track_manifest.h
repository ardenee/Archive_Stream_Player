#pragma once

// Native host track references for players that require an ordinary filename.
// Text manifest format (UTF-8):
//     ARCHIVE_STREAM_NATIVE_TRACK_V1\n
//     <existing archive virtual path>\n
// Only the metadata reference is stored. No compressed or decoded audio bytes.
// These helpers are intentionally limited to the Winamp-style input adapter.
#include <windows.h>
#include <cstring>
#include <string>
#include "archive_reader.h"

inline bool ArchiveNativeTrackHasExtension(const char* filename)
{
    if (!filename)
        return false;
    const char* extension = strrchr(filename, '.');
    return extension && _stricmp(extension, ".ast") == 0;
}

inline bool ArchiveNativeTrackRead(const char* filename, std::string& resolved)
{
    resolved.clear();
    if (!ArchiveNativeTrackHasExtension(filename))
        return false;

    HANDLE file = CreateFileA(filename, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;

    LARGE_INTEGER size = {};
    constexpr DWORD maximum = 16384;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 36 || size.QuadPart > maximum)
    {
        CloseHandle(file);
        return false;
    }

    std::string contents(static_cast<size_t>(size.QuadPart), '\0');
    DWORD count = 0;
    const bool read = ReadFile(file, contents.data(), static_cast<DWORD>(contents.size()), &count, nullptr) != 0;
    CloseHandle(file);
    if (!read || count != contents.size() || contents.find('\0') != std::string::npos)
        return false;

    constexpr char marker[] = "ARCHIVE_STREAM_NATIVE_TRACK_V1\n";
    constexpr size_t markerLength = sizeof(marker) - 1;
    if (contents.compare(0, markerLength, marker) != 0)
        return false;
    std::string virtualPath = contents.substr(markerLength);
    if (!virtualPath.empty() && virtualPath.back() == '\n')
        virtualPath.pop_back();
    if (!virtualPath.empty() && virtualPath.back() == '\r')
        virtualPath.pop_back();
    if (virtualPath.empty() || virtualPath.find('\r') != std::string::npos ||
        virtualPath.find('\n') != std::string::npos || !IsVirtualArchivePath(virtualPath.c_str()))
        return false;

    // Use exactly the same parser as ordinary Winamp virtual album tracks.
    // ParseVirtualArchivePath rejects index/path overflow and malformed aliases.
    char archive[MAX_PATH * 4] = {};
    char internal[1024] = {};
    mz_uint index = 0;
    if (!ParseVirtualArchivePath(virtualPath.c_str(), archive, sizeof(archive),
                                 &index, internal, sizeof(internal)))
        return false;
    if (!IsSupportedArchivePath(archive) || !internal[0])
        return false;

    resolved = std::move(virtualPath);
    return true;
}
