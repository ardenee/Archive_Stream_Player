#define NOMINMAX
#include <windows.h>
#include <new>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <vector>

#include "archive_common.h"
#include "debug_log.h"

bool ArchiveIsValidUtf8(const char* text)
{
    if (!text)
        return false;

    const unsigned char* s = reinterpret_cast<const unsigned char*>(text);

    while (*s)
    {
        if (*s <= 0x7F)
        {
            ++s;
            continue;
        }

        int remaining = 0;

        if ((*s & 0xE0) == 0xC0)
        {
            if (*s < 0xC2)
                return false;

            remaining = 1;
        }
        else if ((*s & 0xF0) == 0xE0)
        {
            remaining = 2;
        }
        else if ((*s & 0xF8) == 0xF0)
        {
            if (*s > 0xF4)
                return false;

            remaining = 3;
        }
        else
        {
            return false;
        }

        ++s;

        for (int i = 0; i < remaining; ++i)
        {
            if ((*s & 0xC0) != 0x80)
                return false;

            ++s;
        }
    }

    return true;
}

static std::string WideToUtf8String(const wchar_t* wide)
{
    if (!wide)
        return {};

    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);

    if (needed <= 0)
    {
        DebugLogWrite("WARN", "COMMON", "WIDE_TO_UTF8_FAIL", "winerr=%lu | reason=size_query_failed", GetLastError());
        return {};
    }

    std::string result;
    result.resize(static_cast<size_t>(needed - 1));

    int converted = WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), needed, nullptr, nullptr);

    if (converted <= 0)
    {
        DebugLogWrite("WARN", "COMMON", "WIDE_TO_UTF8_FAIL", "winerr=%lu | reason=convert_failed", GetLastError());
        return {};
    }

    return result;
}

std::string ArchiveCp437ToUtf8(const char* text)
{
    if (!text)
        return {};

    int neededWide = MultiByteToWideChar(437, MB_ERR_INVALID_CHARS, text, -1, nullptr, 0);

    if (neededWide <= 0)
        neededWide = MultiByteToWideChar(437, 0, text, -1, nullptr, 0);

    if (neededWide <= 0)
    {
        DebugLogWrite("WARN", "COMMON", "CP437_TO_UTF8_FAIL", "winerr=%lu | reason=wide_size_query_failed", GetLastError());
        return text;
    }

    std::vector<wchar_t> wide;

    try
    {
        wide.resize(static_cast<size_t>(neededWide));
    }
    catch (const std::bad_alloc&)
    {
        DebugLogWrite("WARN", "COMMON", "CP437_TO_UTF8_FAIL", "needed_wide=%d | reason=wide_alloc_failed", neededWide);
        return text;
    }

    int converted = MultiByteToWideChar(437, 0, text, -1, wide.data(), neededWide);

    if (converted <= 0)
    {
        DebugLogWrite("WARN", "COMMON", "CP437_TO_UTF8_FAIL", "needed_wide=%d | winerr=%lu | reason=wide_convert_failed", neededWide, GetLastError());
        return text;
    }

    return WideToUtf8String(wide.data());
}

std::string ArchiveZipNameToUtf8BestEffort(const char* zipName)
{
    if (!zipName)
        return {};

    if (ArchiveIsValidUtf8(zipName))
        return zipName;

    return ArchiveCp437ToUtf8(zipName);
}

static std::string ToLowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
        });

    return value;
}

bool ArchiveIsSupportedAudioPath(const std::string& path)
{
    if (path.empty())
        return false;

    std::string clean = path;

    while (!clean.empty())
    {
        unsigned char c = (unsigned char)clean.back();

        if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            clean.pop_back();
            continue;
        }

        break;
    }

    if (clean.empty())
        return false;

    std::string lower = ToLowerAscii(clean);

    const char* exts[] =
    {
        ".mp3",
        ".wav",
        ".flac",
        ".ogg",
        ".m4a",
        ".aac",
        ".wma"
    };

    for (const char* ext : exts)
    {
        size_t extLen = std::strlen(ext);

        if (lower.length() >= extLen && lower.compare(lower.length() - extLen, extLen, ext) == 0)
            return true;
    }

    return false;
}

static bool ArchiveEnsureDirectoryExistsA(const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    if (CreateDirectoryA(path, NULL))
    {
        return true;
    }

    DWORD err = GetLastError();

    if (err == ERROR_ALREADY_EXISTS)
    {
        DWORD attr = GetFileAttributesA(path);

        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY))
        {
            return true;
        }
    }

    return false;
}

bool ArchiveGetPluginDataRoot(char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        return false;
    }

    outPath[0] = '\0';

    char localAppData[MAX_PATH];
    DWORD len = GetEnvironmentVariableA("LOCALAPPDATA", localAppData, sizeof(localAppData));

    if (len == 0 || len >= sizeof(localAppData))
    {
        char userProfile[MAX_PATH];
        DWORD userLen = GetEnvironmentVariableA("USERPROFILE", userProfile, sizeof(userProfile));

        if (userLen == 0 || userLen >= sizeof(userProfile))
        {
            DebugLogWrite("ERROR", "PATH", "PLUGIN_DATA_ROOT_FAIL", "reason=localappdata_and_userprofile_unavailable");
            return false;
        }

        sprintf_s(localAppData, sizeof(localAppData), "%s\\AppData\\Local", userProfile);
    }

    sprintf_s(outPath, outPathSize, "%s\\WinampArchiveStream", localAppData);

    if (!ArchiveEnsureDirectoryExistsA(outPath))
    {
        DebugLogWrite("ERROR", "PATH", "PLUGIN_DATA_ROOT_FAIL", "path=%s | winerr=%lu | reason=create_directory_failed", outPath, GetLastError());
        outPath[0] = '\0';
        return false;
    }

    return true;
}

bool ArchiveBuildPluginDataFilePath(const char* fileName, char* outPath, size_t outPathSize)
{
    if (!fileName || !fileName[0] || !outPath || outPathSize == 0)
    {
        return false;
    }

    char root[MAX_PATH * 4];

    if (!ArchiveGetPluginDataRoot(root, sizeof(root)))
    {
        outPath[0] = '\0';
        return false;
    }

    sprintf_s(outPath, outPathSize, "%s\\%s", root, fileName);
    return true;
}

bool ArchiveBuildPluginDataSubdirPath(const char* subdirName, char* outPath, size_t outPathSize)
{
    if (!subdirName || !subdirName[0] || !outPath || outPathSize == 0)
    {
        return false;
    }

    char root[MAX_PATH * 4];

    if (!ArchiveGetPluginDataRoot(root, sizeof(root)))
    {
        outPath[0] = '\0';
        return false;
    }

    sprintf_s(outPath, outPathSize, "%s\\%s", root, subdirName);

    if (!ArchiveEnsureDirectoryExistsA(outPath))
    {
        DebugLogWrite("ERROR", "PATH", "PLUGIN_DATA_SUBDIR_FAIL", "path=%s | winerr=%lu | reason=create_directory_failed", outPath, GetLastError());
        outPath[0] = '\0';
        return false;
    }

    return true;
}

bool ParseVirtualArchivePath(const char* file, char* outZipPath, size_t outZipPathSize, mz_uint* outIndex, char* outInternalPath, size_t outInternalPathSize)
{
    if (!file || !outZipPath || !outIndex || !outInternalPath)
    {
        DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "reason=invalid_input");
        return false;
    }

    outZipPath[0] = '\0';
    outInternalPath[0] = '\0';
    *outIndex = 0;

    const char* p = nullptr;
    const char* firstPipe = nullptr;
    const char* secondPipe = nullptr;

    if (strncmp(file, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) == 0)
    {
        p = file + strlen(VIRTUAL_PREFIX);

        firstPipe = strchr(p, '|');
        if (!firstPipe)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | reason=missing_first_pipe_prefix_form", file);
            return false;
        }

        secondPipe = strchr(firstPipe + 1, '|');
        if (!secondPipe)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | reason=missing_second_pipe_prefix_form", file);
            return false;
        }

        size_t zipLen = (size_t)(firstPipe - p);

        if (zipLen == 0 || zipLen >= outZipPathSize)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | zip_len=%zu | out_size=%zu | reason=archive_path_too_long_or_empty", file, zipLen, outZipPathSize);
            return false;
        }

        memcpy(outZipPath, p, zipLen);
        outZipPath[zipLen] = '\0';
    }
    else
    {
        const char* marker = strstr(file, ".archive_stream|");
        if (!marker)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | reason=missing_archive_stream_marker", file);
            return false;
        }

        size_t archiveLen = (size_t)(marker - file);
        if (archiveLen == 0 || archiveLen >= outZipPathSize)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | archive_len=%zu | out_size=%zu | reason=archive_path_too_long_or_empty", file, archiveLen, outZipPathSize);
            return false;
        }

        memcpy(outZipPath, file, archiveLen);
        outZipPath[archiveLen] = '\0';

        firstPipe = marker + strlen(".archive_stream");

        if (*firstPipe != '|')
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | reason=missing_pipe_after_marker", file);
            return false;
        }

        secondPipe = strchr(firstPipe + 1, '|');

        if (!secondPipe)
        {
            DebugLogWrite("WARN", "VIRTUAL", "PARSE_FAIL", "file=%s | reason=missing_second_pipe_marker_form", file);
            return false;
        }
    }

    for (char* c = outZipPath; *c; ++c)
    {
        if (*c == '/')
            *c = '\\';
    }

    *outIndex = (mz_uint)strtoul(firstPipe + 1, NULL, 10);

    if (secondPipe[1] != '\0')
        strcpy_s(outInternalPath, outInternalPathSize, secondPipe + 1);

    return true;
}

