#pragma once

// Winamp / WACUP legacy playlist IPC accepts ANSI byte strings, while archive
// filenames and embedded ID3/FLAC/Ogg tags use UTF-8. Convert only at this
// host boundary; never change the archive's stored filename or virtual path.
#include <windows.h>
#include <string>
#include <utility>

inline bool ArchiveWinampUtf8ToAnsi(const std::string& utf8, std::string& ansi, bool rejectUnrepresentable = false)
{
    ansi.clear();
    if (utf8.empty())
        return true;

    const int wideLength = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (wideLength <= 0)
        return false;

    std::wstring wide(static_cast<size_t>(wideLength), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), wideLength) != wideLength)
        return false;

    BOOL substituted = FALSE;
    const int byteLength = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, nullptr, 0, nullptr, &substituted);
    if (byteLength <= 0 || (rejectUnrepresentable && substituted))
        return false;

    std::string encoded(static_cast<size_t>(byteLength), '\0');
    substituted = FALSE;
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(), wideLength, encoded.data(), byteLength, nullptr, &substituted) != byteLength)
        return false;
    if (rejectUnrepresentable && substituted)
        return false;

    ansi = std::move(encoded);
    return true;
}
