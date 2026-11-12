#define NOMINMAX

#ifndef LIBARCHIVE_STATIC
#define LIBARCHIVE_STATIC
#endif

#include "archive_rar_libarchive.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "debug_log.h"
#include "archive_rar_profile_private.h"
#include <archive.h>
#include <archive_entry.h>
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <new>
#include <string>
#include <vector>

struct LibarchiveRarVolume
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t start = 0;
    uint64_t size = 0;
    std::wstring path;
    std::string pathUtf8;
};

struct LibarchiveRarSource
{
    std::vector<LibarchiveRarVolume> volumes;
    uint64_t size = 0;
    uint64_t position = 0;
    std::wstring path;
    std::string pathUtf8;
    unsigned char buffer[256 * 1024]{};
};

struct LaRarHeaderCrcInfo
{
    std::string path;
    uint64_t size = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    bool isFolder = false;
    bool isSolid = true;
    bool hasMethodId = false;
    unsigned int methodId = 0;
};

static bool LaRarWcharToUtf8(const wchar_t* input, std::string& output)
{
    output.clear();

    if (!input || !input[0])
    {
        return false;
    }

    int needed = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);

    if (needed <= 1)
    {
        return false;
    }

    output.resize((size_t)needed - 1);

    int written = WideCharToMultiByte(CP_UTF8, 0, input, -1, &output[0], needed, NULL, NULL);
    return written > 0;
}

static bool LaRarIsSupportedAudioPath(const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    const char* ext = strrchr(path, '.');

    if (!ext)
    {
        return false;
    }

    return _stricmp(ext, ".mp3") == 0 ||
        _stricmp(ext, ".wav") == 0 ||
        _stricmp(ext, ".flac") == 0 ||
        _stricmp(ext, ".ogg") == 0 ||
        _stricmp(ext, ".wma") == 0 ||
        _stricmp(ext, ".m4a") == 0 ||
        _stricmp(ext, ".aac") == 0;
}

static bool LaRarIsRegularFilePath(const wchar_t* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    DWORD attrs = GetFileAttributesW(path);

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        return false;
    }

    return (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool LaRarGetFileSize64(const wchar_t* path, uint64_t* outSize)
{
    if (outSize)
    {
        *outSize = 0;
    }

    if (!path || !path[0] || !outSize)
    {
        return false;
    }

    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (file == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    LARGE_INTEGER fileSize;
    BOOL ok = GetFileSizeEx(file, &fileSize);
    CloseHandle(file);

    if (!ok || fileSize.QuadPart < 0)
    {
        return false;
    }

    *outSize = (uint64_t)fileSize.QuadPart;
    return true;
}

static bool LaRarTryParsePartNumberPath(const wchar_t* archivePath, std::wstring& prefix, std::wstring& suffix, int& partNumber, size_t& digitWidth)
{
    prefix.clear();
    suffix.clear();
    partNumber = 0;
    digitWidth = 0;

    if (!archivePath || !archivePath[0])
    {
        return false;
    }

    std::wstring path(archivePath);

    if (path.length() < 12)
    {
        return false;
    }

    size_t extPos = path.length() - 4;

    if (_wcsicmp(path.c_str() + extPos, L".rar") != 0)
    {
        return false;
    }

    size_t slashPos = path.find_last_of(L"\\/");
    size_t fileStart = (slashPos == std::wstring::npos) ? 0 : slashPos + 1;
    size_t partPos = path.rfind(L".part", extPos);

    if (partPos == std::wstring::npos || partPos < fileStart)
    {
        return false;
    }

    size_t digitsStart = partPos + 5;
    size_t digitsEnd = extPos;

    if (digitsStart >= digitsEnd)
    {
        return false;
    }

    for (size_t i = digitsStart; i < digitsEnd; ++i)
    {
        if (path[i] < L'0' || path[i] > L'9')
        {
            return false;
        }
    }

    std::wstring digits = path.substr(digitsStart, digitsEnd - digitsStart);
    int parsed = _wtoi(digits.c_str());

    if (parsed <= 0)
    {
        return false;
    }

    prefix = path.substr(0, digitsStart);
    suffix = path.substr(digitsEnd);
    partNumber = parsed;
    digitWidth = digits.length();
    return true;
}

static std::wstring LaRarBuildPartNumberPath(const std::wstring& prefix, int partNumber, size_t digitWidth, const std::wstring& suffix)
{
    wchar_t numberText[32];

    if (digitWidth > 0 && digitWidth < 30)
    {
        swprintf_s(numberText, sizeof(numberText) / sizeof(numberText[0]), L"%0*d", (int)digitWidth, partNumber);
    }
    else
    {
        swprintf_s(numberText, sizeof(numberText) / sizeof(numberText[0]), L"%d", partNumber);
    }

    return prefix + numberText + suffix;
}

static void LaRarDiscoverPartNumberVolumes(const wchar_t* archivePath, std::vector<std::wstring>& volumePaths)
{
    volumePaths.clear();

    std::wstring prefix;
    std::wstring suffix;
    int partNumber = 0;
    size_t digitWidth = 0;

    if (!LaRarTryParsePartNumberPath(archivePath, prefix, suffix, partNumber, digitWidth))
    {
        volumePaths.push_back(archivePath);
        return;
    }

    int firstPart = partNumber;

    while (firstPart > 1)
    {
        std::wstring previousPath = LaRarBuildPartNumberPath(prefix, firstPart - 1, digitWidth, suffix);

        if (!LaRarIsRegularFilePath(previousPath.c_str()))
        {
            break;
        }

        --firstPart;
    }

    for (int currentPart = firstPart; currentPart < 1000000; ++currentPart)
    {
        std::wstring currentPath = LaRarBuildPartNumberPath(prefix, currentPart, digitWidth, suffix);

        if (!LaRarIsRegularFilePath(currentPath.c_str()))
        {
            break;
        }

        volumePaths.push_back(currentPath);
    }

    if (volumePaths.empty())
    {
        volumePaths.push_back(archivePath);
    }
}

static LibarchiveRarVolume* LaRarFindVolumeForPosition(LibarchiveRarSource* source, uint64_t position)
{
    if (!source)
    {
        return nullptr;
    }

    for (LibarchiveRarVolume& volume : source->volumes)
    {
        if (position >= volume.start && position < volume.start + volume.size)
        {
            return &volume;
        }
    }

    return nullptr;
}

static void LaRarCloseSource(LibarchiveRarSource* source)
{
    if (!source)
    {
        return;
    }

    for (LibarchiveRarVolume& volume : source->volumes)
    {
        if (volume.handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(volume.handle);
            volume.handle = INVALID_HANDLE_VALUE;
        }

        volume.start = 0;
        volume.size = 0;
        volume.path.clear();
        volume.pathUtf8.clear();
    }

    source->volumes.clear();
    source->size = 0;
    source->position = 0;
    source->path.clear();
    source->pathUtf8.clear();
}

static bool LaRarOpenSource(const wchar_t* archivePath, LibarchiveRarSource* source, std::string& error)
{
    if (!archivePath || !archivePath[0] || !source)
    {
        error = "Invalid RAR source open input";
        return false;
    }

    LaRarCloseSource(source);

    source->path = archivePath;

    if (!LaRarWcharToUtf8(archivePath, source->pathUtf8))
    {
        error = "Failed to convert RAR path to UTF-8";
        DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "reason=path_convert_failed");
        return false;
    }

    std::vector<std::wstring> volumePaths;
    LaRarDiscoverPartNumberVolumes(archivePath, volumePaths);

    uint64_t runningOffset = 0;

    for (const std::wstring& volumePath : volumePaths)
    {
        std::string volumePathUtf8;

        if (!LaRarWcharToUtf8(volumePath.c_str(), volumePathUtf8))
        {
            error = "Failed to convert RAR volume path to UTF-8";
            DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | reason=volume_path_convert_failed", source->pathUtf8.c_str());
            LaRarCloseSource(source);
            return false;
        }

        DWORD attrs = GetFileAttributesW(volumePath.c_str());

        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            error = "RAR volume is missing";
            DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | volume=%s | reason=missing | winerr=%lu", source->pathUtf8.c_str(), volumePathUtf8.c_str(), GetLastError());
            LaRarCloseSource(source);
            return false;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            error = "RAR volume path is a directory";
            DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | volume=%s | reason=is_directory", source->pathUtf8.c_str(), volumePathUtf8.c_str());
            LaRarCloseSource(source);
            return false;
        }

        HANDLE handle = CreateFileW(volumePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "Failed to open RAR volume";
            DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | volume=%s | reason=open_failed | winerr=%lu", source->pathUtf8.c_str(), volumePathUtf8.c_str(), GetLastError());
            LaRarCloseSource(source);
            return false;
        }

        LARGE_INTEGER fileSize;

        if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0)
        {
            error = "Failed to get RAR volume size";
            DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | volume=%s | reason=get_size_failed | winerr=%lu", source->pathUtf8.c_str(), volumePathUtf8.c_str(), GetLastError());
            CloseHandle(handle);
            LaRarCloseSource(source);
            return false;
        }

        LibarchiveRarVolume volume;
        volume.handle = handle;
        volume.start = runningOffset;
        volume.size = (uint64_t)fileSize.QuadPart;
        volume.path = volumePath;
        volume.pathUtf8 = volumePathUtf8;

        runningOffset += volume.size;
        source->volumes.push_back(volume);
    }

    if (source->volumes.empty() || runningOffset == 0)
    {
        error = "No readable RAR volumes found";
        DebugLogWrite("ERROR", "LARAR", "OPEN_SOURCE_FAIL", "archive=%s | reason=no_readable_volumes", source->pathUtf8.c_str());
        LaRarCloseSource(source);
        return false;
    }

    source->size = runningOffset;
    source->position = 0;

    if (source->volumes.size() > 1)
    {
        //DebugLogWrite("INFO", "LARAR", "MULTIPART_SOURCE_OPEN", "archive=%s | volumes=%zu | total_size=%llu | first=%s | last=%s", source->pathUtf8.c_str(), source->volumes.size(), (unsigned long long)source->size, source->volumes.front().pathUtf8.c_str(), source->volumes.back().pathUtf8.c_str());
    }

    return true;
}

static la_ssize_t LaRarReadCallback(struct archive* archiveHandle, void* clientData, const void** buffer)
{
    (void)archiveHandle;

    LibarchiveRarSource* source = static_cast<LibarchiveRarSource*>(clientData);

    if (!source || !buffer)
    {
        return ARCHIVE_FATAL;
    }

    if (source->position >= source->size)
    {
        *buffer = source->buffer;
        return 0;
    }

    LibarchiveRarVolume* volume = LaRarFindVolumeForPosition(source, source->position);

    if (!volume || volume->handle == INVALID_HANDLE_VALUE)
    {
        DebugLogWrite("ERROR", "LARAR", "READ_FAIL", "archive=%s | offset=%llu | reason=no_volume_for_position", source->pathUtf8.c_str(), (unsigned long long)source->position);
        return ARCHIVE_FATAL;
    }

    uint64_t volumeOffset = source->position - volume->start;
    uint64_t remainingInVolume = volume->size - volumeOffset;
    uint64_t remainingTotal = source->size - source->position;
    DWORD wanted = (DWORD)std::min((uint64_t)sizeof(source->buffer), std::min(remainingInVolume, remainingTotal));

    LARGE_INTEGER seekPos;
    seekPos.QuadPart = (LONGLONG)volumeOffset;

    if (!SetFilePointerEx(volume->handle, seekPos, NULL, FILE_BEGIN))
    {
        DebugLogWrite("ERROR", "LARAR", "READ_FAIL", "archive=%s | volume=%s | offset=%llu | volume_offset=%llu | reason=seek_failed | winerr=%lu", source->pathUtf8.c_str(), volume->pathUtf8.c_str(), (unsigned long long)source->position, (unsigned long long)volumeOffset, GetLastError());
        return ARCHIVE_FATAL;
    }

    DWORD bytesRead = 0;

    if (!ReadFile(volume->handle, source->buffer, wanted, &bytesRead, NULL))
    {
        DebugLogWrite("ERROR", "LARAR", "READ_FAIL", "archive=%s | volume=%s | offset=%llu | volume_offset=%llu | requested=%lu | reason=read_failed | winerr=%lu", source->pathUtf8.c_str(), volume->pathUtf8.c_str(), (unsigned long long)source->position, (unsigned long long)volumeOffset, (unsigned long)wanted, GetLastError());
        return ARCHIVE_FATAL;
    }

    source->position += bytesRead;
    *buffer = source->buffer;
    return (la_ssize_t)bytesRead;
}

static la_int64_t LaRarSeekCallback(struct archive* archiveHandle, void* clientData, la_int64_t offset, int whence)
{
    (void)archiveHandle;

    LibarchiveRarSource* source = static_cast<LibarchiveRarSource*>(clientData);

    if (!source)
    {
        return -1;
    }

    int64_t newPos = 0;

    if (whence == SEEK_SET)
    {
        newPos = (int64_t)offset;
    }
    else if (whence == SEEK_CUR)
    {
        newPos = (int64_t)source->position + (int64_t)offset;
    }
    else if (whence == SEEK_END)
    {
        newPos = (int64_t)source->size + (int64_t)offset;
    }
    else
    {
        DebugLogWrite("ERROR", "LARAR", "SEEK_FAIL", "archive=%s | offset=%lld | whence=%d | reason=unsupported_whence", source->pathUtf8.c_str(), (long long)offset, whence);
        return -1;
    }

    if (newPos < 0)
    {
        DebugLogWrite("ERROR", "LARAR", "SEEK_FAIL", "archive=%s | offset=%lld | whence=%d | reason=negative_position", source->pathUtf8.c_str(), (long long)offset, whence);
        return -1;
    }

    source->position = (uint64_t)newPos;
    return (la_int64_t)source->position;
}

static la_int64_t LaRarSkipCallback(struct archive* archiveHandle, void* clientData, la_int64_t request)
{
    (void)archiveHandle;

    LibarchiveRarSource* source = static_cast<LibarchiveRarSource*>(clientData);

    if (!source || request < 0)
    {
        return 0;
    }

    uint64_t available = 0;

    if (source->position < source->size)
    {
        available = source->size - source->position;
    }

    uint64_t skip = (uint64_t)request;

    if (skip > available)
    {
        skip = available;
    }

    source->position += skip;
    return (la_int64_t)skip;
}

static int LaRarCloseCallback(struct archive* archiveHandle, void* clientData)
{
    (void)archiveHandle;

    LibarchiveRarSource* source = static_cast<LibarchiveRarSource*>(clientData);
    LaRarCloseSource(source);

    return ARCHIVE_OK;
}

static int LaRarAddSavedPasswords(struct archive* archiveHandle, const std::string& archivePathUtf8)
{
    if (!archiveHandle)
    {
        return 0;
    }

    ArchivePasswordList passwordList;
    std::string passwordError;

    if (!ArchivePasswordLoadList(passwordList, passwordError))
    {
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "archive=%s | backend=libarchive_rar | path=%s | error=%s", archivePathUtf8.c_str(), ArchivePasswordGetListPath(), passwordError.c_str());
        return 0;
    }

    int added = 0;

    for (const std::string& password : passwordList.passwords)
    {
        if (password.empty())
        {
            continue;
        }

        if (archive_read_add_passphrase(archiveHandle, password.c_str()) == ARCHIVE_OK)
        {
            ++added;
        }
    }

    std::vector<std::string> runtimePasswords;
    ArchivePasswordGetRuntimePasswords(archivePathUtf8.c_str(), runtimePasswords);

    for (const std::string& password : runtimePasswords)
    {
        if (password.empty())
        {
            continue;
        }

        if (archive_read_add_passphrase(archiveHandle, password.c_str()) == ARCHIVE_OK)
        {
            ++added;
        }
    }

    if (!passwordList.passwords.empty())
    {
        //DebugLogWrite("INFO", "PASSWORD", "CANDIDATES_LOADED", "archive=%s | backend=libarchive_rar | path=%s | count=%zu", archivePathUtf8.c_str(), ArchivePasswordGetListPath(), passwordList.passwords.size());

    }

    if (!runtimePasswords.empty())
    {
        //DebugLogWrite("INFO", "PASSWORD", "RUNTIME_CANDIDATES_LOADED", "archive=%s | backend=libarchive_rar | count=%zu", archivePathUtf8.c_str(), runtimePasswords.size());

    }

    return added;
}
static uint16_t LaRarReadLe16(const unsigned char* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t LaRarReadLe32(const unsigned char* p)
{
    return (uint32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static bool LaRarReadExact(HANDLE file, void* buffer, DWORD bytesToRead);

static bool LaRarReadVintFile(HANDLE file, uint64_t& value)
{
    value = 0;

    for (unsigned int shift = 0; shift < 70; shift += 7)
    {
        unsigned char byte = 0;

        if (!LaRarReadExact(file, &byte, 1))
        {
            return false;
        }

        value |= ((uint64_t)(byte & 0x7F) << shift);

        if ((byte & 0x80) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool LaRarReadVintBuffer(const unsigned char* data, size_t size, size_t& offset, uint64_t& value)
{
    value = 0;

    for (unsigned int shift = 0; shift < 70; shift += 7)
    {
        if (offset >= size)
        {
            return false;
        }

        unsigned char byte = data[offset++];
        value |= ((uint64_t)(byte & 0x7F) << shift);

        if ((byte & 0x80) == 0)
        {
            return true;
        }
    }

    return false;
}

static bool LaRarReadExact(HANDLE file, void* buffer, DWORD bytesToRead)
{
    DWORD bytesRead = 0;
    return ReadFile(file, buffer, bytesToRead, &bytesRead, NULL) && bytesRead == bytesToRead;
}

static bool LaRarSeekCurrent(HANDLE file, uint64_t bytesToSkip)
{
    LARGE_INTEGER distance;
    distance.QuadPart = (LONGLONG)bytesToSkip;
    return SetFilePointerEx(file, distance, NULL, FILE_CURRENT) != FALSE;
}

static void LaRarNormalizeHeaderPath(std::string& path)
{
    for (char& ch : path)
    {
        if (ch == '\\')
        {
            ch = '/';
        }
    }
}

static bool LaRarFindHeaderCrc(const std::vector<LaRarHeaderCrcInfo>& crcMap, const std::string& path, uint64_t size, uint32_t& crc32)
{
    for (const LaRarHeaderCrcInfo& item : crcMap)
    {
        if (item.hasCrc32 && item.path == path)
        {
            crc32 = item.crc32;
            return true;
        }
    }

    for (const LaRarHeaderCrcInfo& item : crcMap)
    {
        if (item.hasCrc32 && item.size == size && _stricmp(item.path.c_str(), path.c_str()) == 0)
        {
            crc32 = item.crc32;
            return true;
        }
    }

    return false;
}

static bool LaRarBuildRar5HeaderCrcMap(const LibarchiveRarSource& source, std::vector<LaRarHeaderCrcInfo>& crcMap, std::string& error)
{
    crcMap.clear();
    error.clear();

    static const unsigned char rar5Signature[8] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x01, 0x00 };

    const uint64_t headerFlagExtraArea = 0x0001;
    const uint64_t headerFlagDataArea = 0x0002;
    const uint64_t headerFlagSplitAfter = 0x0010;

    const uint64_t headerTypeFile = 2;
    const uint64_t headerTypeEnd = 5;

    const uint64_t fileFlagDirectory = 0x0001;
    const uint64_t fileFlagUnixMtime = 0x0002;
    const uint64_t fileFlagCrc32 = 0x0004;

    for (const LibarchiveRarVolume& volume : source.volumes)
    {
        if (volume.handle == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        LARGE_INTEGER startPos;
        startPos.QuadPart = 0;

        if (!SetFilePointerEx(volume.handle, startPos, NULL, FILE_BEGIN))
        {
            error = "Failed to seek RAR5 volume";
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_VOLUME_SKIP", "volume=%s | reason=seek_start_failed | winerr=%lu", volume.pathUtf8.c_str(), GetLastError());
            continue;
        }

        unsigned char signature[8];

        if (!LaRarReadExact(volume.handle, signature, sizeof(signature)))
        {
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_VOLUME_SKIP", "volume=%s | reason=signature_read_failed", volume.pathUtf8.c_str());
            continue;
        }

        if (memcmp(signature, rar5Signature, sizeof(signature)) != 0)
        {
            //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_RAR5_VOLUME_SKIP", "volume=%s | reason=not_rar5_signature", volume.pathUtf8.c_str());
            continue;
        }

        for (;;)
        {
            unsigned char headerCrcBytes[4];

            if (!LaRarReadExact(volume.handle, headerCrcBytes, sizeof(headerCrcBytes)))
            {
                break;
            }

            uint64_t headerSize = 0;

            if (!LaRarReadVintFile(volume.handle, headerSize))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=header_size_read_failed", volume.pathUtf8.c_str());
                break;
            }

            if (headerSize == 0 || headerSize > (2 * 1024 * 1024))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=invalid_header_size | header_size=%llu", volume.pathUtf8.c_str(), (unsigned long long)headerSize);
                break;
            }

            std::vector<unsigned char> headerData;
            headerData.resize((size_t)headerSize);

            if (!LaRarReadExact(volume.handle, headerData.data(), (DWORD)headerData.size()))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=header_read_failed | header_size=%llu", volume.pathUtf8.c_str(), (unsigned long long)headerSize);
                break;
            }

            size_t offset = 0;
            uint64_t headerType = 0;
            uint64_t headerFlags = 0;
            uint64_t extraAreaSize = 0;
            uint64_t dataSize = 0;

            if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, headerType))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=header_type_read_failed", volume.pathUtf8.c_str());
                break;
            }

            if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, headerFlags))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=header_flags_read_failed", volume.pathUtf8.c_str());
                break;
            }

            if (headerFlags & headerFlagExtraArea)
            {
                if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, extraAreaSize))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=extra_area_size_read_failed", volume.pathUtf8.c_str());
                    break;
                }
            }

            if (headerFlags & headerFlagDataArea)
            {
                if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, dataSize))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=data_size_read_failed", volume.pathUtf8.c_str());
                    break;
                }
            }

            if (headerType == headerTypeFile)
            {
                uint64_t fileFlags = 0;
                uint64_t unpackedSize = 0;
                uint64_t attributes = 0;
                uint32_t fileCrc = 0;
                bool hasFileCrc = false;
                uint64_t compressionInfo = 0;
                uint64_t hostOs = 0;
                uint64_t nameSize = 0;

                if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, fileFlags) ||
                    !LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, unpackedSize) ||
                    !LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, attributes))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_FILE_SKIP", "volume=%s | reason=base_file_fields_read_failed", volume.pathUtf8.c_str());
                }
                else
                {
                    if (fileFlags & fileFlagUnixMtime)
                    {
                        if (offset + 4 > headerData.size())
                        {
                            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_FILE_SKIP", "volume=%s | reason=mtime_out_of_range", volume.pathUtf8.c_str());
                            goto rar5_skip_data_area;
                        }

                        offset += 4;
                    }

                    if (fileFlags & fileFlagCrc32)
                    {
                        if (offset + 4 > headerData.size())
                        {
                            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_FILE_SKIP", "volume=%s | reason=crc_out_of_range", volume.pathUtf8.c_str());
                            goto rar5_skip_data_area;
                        }

                        fileCrc = LaRarReadLe32(headerData.data() + offset);
                        offset += 4;
                        hasFileCrc = true;
                    }

                    if (!LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, compressionInfo) ||
                        !LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, hostOs) ||
                        !LaRarReadVintBuffer(headerData.data(), headerData.size(), offset, nameSize))
                    {
                        DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_FILE_SKIP", "volume=%s | reason=tail_file_fields_read_failed", volume.pathUtf8.c_str());
                        goto rar5_skip_data_area;
                    }

                    if (nameSize > headerData.size() || offset + (size_t)nameSize > headerData.size())
                    {
                        DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_FILE_SKIP", "volume=%s | reason=name_out_of_range | name_size=%llu | header_size=%zu", volume.pathUtf8.c_str(), (unsigned long long)nameSize, headerData.size());
                        goto rar5_skip_data_area;
                    }

                    std::string headerPath((const char*)headerData.data() + offset, (size_t)nameSize);
                    size_t nulPos = headerPath.find('\0');

                    if (nulPos != std::string::npos)
                    {
                        headerPath.resize(nulPos);
                    }

                    LaRarNormalizeHeaderPath(headerPath);

                    const bool isDirectory = (fileFlags & fileFlagDirectory) != 0;
                    const bool splitAfter = (headerFlags & headerFlagSplitAfter) != 0;

                    if (!headerPath.empty() && !splitAfter)
                    {
                        LaRarHeaderCrcInfo info;
                        info.path = headerPath;
                        info.size = isDirectory ? 0 : unpackedSize;
                        info.crc32 = fileCrc;
                        info.hasCrc32 = !isDirectory && hasFileCrc;
                        info.isFolder = isDirectory;
                        info.isSolid = true;
                        info.hasMethodId = true;
                        info.methodId = (unsigned int)(compressionInfo & 0x7);
                        crcMap.push_back(info);
                    }
                    else
                    {
                        //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_RAR5_MAP_SKIP", "volume=%s | entry=%s | dir=%d | has_crc=%d | split_after=%d | header_flags=%llu | file_flags=%llu", volume.pathUtf8.c_str(), headerPath.c_str(), isDirectory ? 1 : 0, hasFileCrc ? 1 : 0, splitAfter ? 1 : 0, (unsigned long long)headerFlags, (unsigned long long)fileFlags);
                    }
                }
            }

        rar5_skip_data_area:
            if (dataSize > 0 && !LaRarSeekCurrent(volume.handle, dataSize))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_RAR5_SCAN_STOP", "volume=%s | reason=data_seek_failed | bytes=%llu | winerr=%lu", volume.pathUtf8.c_str(), (unsigned long long)dataSize, GetLastError());
                break;
            }

            if (headerType == headerTypeEnd)
            {
                break;
            }
        }
    }

    //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_RAR5_MAP_DONE", "archive=%s | entries=%zu", source.pathUtf8.c_str(), crcMap.size());
    return !crcMap.empty();
}

static bool LaRarBuildRar3HeaderCrcMap(const LibarchiveRarSource& source, std::vector<LaRarHeaderCrcInfo>& crcMap, std::string& error)
{
    crcMap.clear();
    error.clear();

    static const unsigned char rar3Signature[7] = { 0x52, 0x61, 0x72, 0x21, 0x1A, 0x07, 0x00 };

    const unsigned char blockFile = 0x74;
    const unsigned char blockEndArc = 0x7B;
    const uint16_t longBlock = 0x8000;
    const uint16_t fileSplitAfter = 0x0002;
    const uint16_t fileLarge = 0x0100;

    for (const LibarchiveRarVolume& volume : source.volumes)
    {
        if (volume.handle == INVALID_HANDLE_VALUE)
        {
            continue;
        }

        LARGE_INTEGER startPos;
        startPos.QuadPart = 0;

        if (!SetFilePointerEx(volume.handle, startPos, NULL, FILE_BEGIN))
        {
            error = "Failed to seek RAR volume";
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_VOLUME_SKIP", "volume=%s | reason=seek_start_failed | winerr=%lu", volume.pathUtf8.c_str(), GetLastError());
            continue;
        }

        unsigned char signature[7];

        if (!LaRarReadExact(volume.handle, signature, sizeof(signature)))
        {
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_VOLUME_SKIP", "volume=%s | reason=signature_read_failed", volume.pathUtf8.c_str());
            continue;
        }

        if (memcmp(signature, rar3Signature, sizeof(signature)) != 0)
        {
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_VOLUME_SKIP", "volume=%s | reason=not_rar3_signature", volume.pathUtf8.c_str());
            continue;
        }

        for (;;)
        {
            unsigned char baseHeader[7];

            if (!LaRarReadExact(volume.handle, baseHeader, sizeof(baseHeader)))
            {
                break;
            }

            unsigned char headType = baseHeader[2];
            uint16_t headFlags = LaRarReadLe16(baseHeader + 3);
            uint16_t headSize = LaRarReadLe16(baseHeader + 5);

            if (headSize < sizeof(baseHeader))
            {
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_STOP", "volume=%s | reason=invalid_head_size | head_type=%u | head_size=%u", volume.pathUtf8.c_str(), (unsigned int)headType, (unsigned int)headSize);
                break;
            }

            uint32_t remainingHeader = (uint32_t)headSize - 7;
            std::vector<unsigned char> headerData;

            if (remainingHeader > 0)
            {
                headerData.resize(remainingHeader);

                if (!LaRarReadExact(volume.handle, headerData.data(), remainingHeader))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_STOP", "volume=%s | reason=header_read_failed | head_type=%u | remaining=%u", volume.pathUtf8.c_str(), (unsigned int)headType, (unsigned int)remainingHeader);
                    break;
                }
            }

            if (headType == blockFile)
            {
                if (headerData.size() < 25)
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_FILE_SKIP", "volume=%s | reason=short_file_header | size=%u", volume.pathUtf8.c_str(), (unsigned int)headerData.size());
                    continue;
                }

                uint32_t packSizeLow = LaRarReadLe32(headerData.data() + 0);
                uint32_t unpackedSizeLow = LaRarReadLe32(headerData.data() + 4);
                uint32_t fileCrc = LaRarReadLe32(headerData.data() + 9);
                uint16_t nameSize = LaRarReadLe16(headerData.data() + 19);

                uint64_t packedSize = packSizeLow;
                uint64_t unpackedSize = unpackedSizeLow;
                size_t nameOffset = 25;

                if (headFlags & fileLarge)
                {
                    if (headerData.size() < 33)
                    {
                        DebugLogWrite("WARN", "LARAR", "HEADER_CRC_FILE_SKIP", "volume=%s | reason=short_large_file_header | size=%u", volume.pathUtf8.c_str(), (unsigned int)headerData.size());
                        continue;
                    }

                    uint32_t highPackSize = LaRarReadLe32(headerData.data() + 25);
                    uint32_t highUnpackedSize = LaRarReadLe32(headerData.data() + 29);
                    packedSize |= ((uint64_t)highPackSize << 32);
                    unpackedSize |= ((uint64_t)highUnpackedSize << 32);
                    nameOffset = 33;
                }

                if (nameOffset + nameSize <= headerData.size())
                {
                    std::string headerPath((const char*)headerData.data() + nameOffset, (size_t)nameSize);
                    size_t nulPos = headerPath.find('\0');

                    if (nulPos != std::string::npos)
                    {
                        headerPath.resize(nulPos);
                    }

                    LaRarNormalizeHeaderPath(headerPath);

                    if (!headerPath.empty() && (headFlags & fileSplitAfter) == 0)
                    {
                        LaRarHeaderCrcInfo info;
                        info.path = headerPath;
                        info.size = unpackedSize;
                        info.crc32 = fileCrc;
                        info.hasCrc32 = true;
                        crcMap.push_back(info);
                        //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_MAP_ENTRY", "volume=%s | entry=%s | size=%llu | crc=%08X | flags=%04X", volume.pathUtf8.c_str(), info.path.c_str(), (unsigned long long)info.size, (unsigned int)info.crc32, (unsigned int)headFlags);
                    }
                    else
                    {
                        //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_MAP_SKIP", "volume=%s | entry=%s | split_after=%d | flags=%04X", volume.pathUtf8.c_str(), headerPath.c_str(), (headFlags & fileSplitAfter) ? 1 : 0, (unsigned int)headFlags);
                    }
                }
                else
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_FILE_SKIP", "volume=%s | reason=name_out_of_range | name_offset=%u | name_size=%u | header_size=%u", volume.pathUtf8.c_str(), (unsigned int)nameOffset, (unsigned int)nameSize, (unsigned int)headerData.size());
                }

                if (!LaRarSeekCurrent(volume.handle, packedSize))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_STOP", "volume=%s | reason=packed_seek_failed | packed=%llu | winerr=%lu", volume.pathUtf8.c_str(), (unsigned long long)packedSize, GetLastError());
                    break;
                }
            }
            else
            {
                uint64_t dataSizeToSkip = 0;

                if ((headFlags & longBlock) && headerData.size() >= 4)
                {
                    dataSizeToSkip = LaRarReadLe32(headerData.data());
                }

                if (dataSizeToSkip > 0 && !LaRarSeekCurrent(volume.handle, dataSizeToSkip))
                {
                    DebugLogWrite("WARN", "LARAR", "HEADER_CRC_SCAN_STOP", "volume=%s | reason=block_data_seek_failed | head_type=%u | bytes=%llu | winerr=%lu", volume.pathUtf8.c_str(), (unsigned int)headType, (unsigned long long)dataSizeToSkip, GetLastError());
                    break;
                }
            }

            if (headType == blockEndArc)
            {
                break;
            }
        }
    }

    //DebugLogWrite("INFO", "LARAR", "HEADER_CRC_MAP_DONE", "archive=%s | entries=%zu", source.pathUtf8.c_str(), crcMap.size());
    return !crcMap.empty();
}

static bool LaRarBuildHeaderCrcMap(const LibarchiveRarSource& source, std::vector<LaRarHeaderCrcInfo>& crcMap, std::string& error)
{
    if (LaRarBuildRar5HeaderCrcMap(source, crcMap, error))
    {
        return true;
    }

    std::string rar3Error;

    if (LaRarBuildRar3HeaderCrcMap(source, crcMap, rar3Error))
    {
        error.clear();
        return true;
    }

    if (error.empty())
    {
        error = rar3Error.empty() ? "No RAR3/RAR5 header CRC entries found" : rar3Error;
    }

    return false;
}

static bool LaRarOpenArchive(struct archive** outArchive, LibarchiveRarSource* source, const wchar_t* archivePath, const std::vector<std::string>* runtimePasswords, std::string& error)
{
    if (outArchive)
    {
        *outArchive = nullptr;
    }

    if (!outArchive || !source)
    {
        error = "Invalid libarchive RAR open inputs";
        return false;
    }

    if (!LaRarOpenSource(archivePath, source, error))
    {
        return false;
    }

    struct archive* a = archive_read_new();

    if (!a)
    {
        error = "archive_read_new failed";
        DebugLogWrite("ERROR", "LARAR", "OPEN_FAIL", "archive=%s | reason=archive_read_new_failed", source->pathUtf8.c_str());
        LaRarCloseSource(source);
        return false;
    }

    archive_read_support_filter_none(a);
    archive_read_support_format_rar(a);
    archive_read_support_format_rar5(a);

    int passwordCandidateCount = LaRarAddSavedPasswords(a, source->pathUtf8);

    if (runtimePasswords)
    {
        for (const std::string& password : *runtimePasswords)
        {
            if (password.empty())
            {
                continue;
            }

            if (archive_read_add_passphrase(a, password.c_str()) == ARCHIVE_OK)
            {
                ++passwordCandidateCount;
            }
        }
    }

    (void)passwordCandidateCount;

    if (archive_read_set_seek_callback(a, LaRarSeekCallback) != ARCHIVE_OK)
    {
        const char* err = archive_error_string(a);
        error = err ? err : "archive_read_set_seek_callback failed";
        DebugLogWrite("ERROR", "LARAR", "OPEN_FAIL", "archive=%s | reason=set_seek_callback_failed | error=%s", source->pathUtf8.c_str(), error.c_str());
        archive_read_free(a);
        LaRarCloseSource(source);
        return false;
    }

    int openResult = archive_read_open2(a, source, NULL, LaRarReadCallback, LaRarSkipCallback, LaRarCloseCallback);

    if (openResult != ARCHIVE_OK)
    {
        const char* err = archive_error_string(a);
        error = err ? err : "archive_read_open2 failed";
        DebugLogWrite("ERROR", "LARAR", "OPEN_FAIL", "archive=%s | reason=archive_read_open2_failed | error=%s", source->pathUtf8.c_str(), error.c_str());
        archive_read_free(a);
        LaRarCloseSource(source);
        return false;
    }

    *outArchive = a;
    return true;
}

static bool LaRarApplyCurrentEntryProfile(struct archive* archiveHandle, ArchiveEntry& entry, bool supportedAudio, bool archiveIsMultipart)
{
    if (!archiveHandle)
    {
        return false;
    }

    archive_rar_entry_profile profile;
    memset(&profile, 0, sizeof(profile));

    int profileResult = archive_read_format_rar_current_entry_profile(archiveHandle, &profile);

    if (profileResult != ARCHIVE_OK)
    {
        profileResult = archive_read_format_rar5_current_entry_profile(archiveHandle, &profile);
    }

    if (profileResult != ARCHIVE_OK)
    {
        entry.hasCrc32 = false;
        entry.crc32 = 0;
        entry.isSolid = true;
        entry.folderIndex = 0xFFFFFFFF;
        entry.filesInFolder = 1;
        entry.hasMethodId = false;
        entry.methodId = 0;
        entry.streamCandidate = false;
        return false;
    }

    const bool splitEntry = profile.split_before != 0 || profile.split_after != 0;
    const bool crcHeaderIsUsable = profile.has_crc32 && !archiveIsMultipart && !splitEntry;

    if (crcHeaderIsUsable)
    {
        entry.hasCrc32 = true;
        entry.crc32 = profile.crc32;
    }
    else
    {
        entry.hasCrc32 = false;
        entry.crc32 = 0;
    }

    if (profile.has_solid)
    {
        entry.isSolid = profile.solid != 0;
    }
    else
    {
        entry.isSolid = true;
    }

    entry.folderIndex = 0xFFFFFFFF;
    entry.filesInFolder = 1;

    if (profile.has_method)
    {
        entry.hasMethodId = true;
        entry.methodId = profile.method;
    }
    else
    {
        entry.hasMethodId = false;
        entry.methodId = 0;
    }

    /*
        Match the old UnRAR route policy:
        - supported audio only
        - no folders
        - non-solid entries only

        Password/missing-volume cases still fail cleanly at stream open/read.
    */
    entry.streamCandidate = supportedAudio && !entry.isFolder && !entry.isSolid;

    //DebugLogWrite("INFO", "LARAR", "ENTRY_PROFILE", "index=%u | crc=%08X | has_crc=%d | raw_crc=%08X | raw_has_crc=%d | crc_usable=%d | archive_multipart=%d | split_before=%d | split_after=%d | solid=%d | has_solid=%d | method=%u | has_method=%d | supported_audio=%d | stream_candidate=%d", (unsigned int)entry.index, (unsigned int)entry.crc32, entry.hasCrc32 ? 1 : 0, (unsigned int)profile.crc32, profile.has_crc32 ? 1 : 0, crcHeaderIsUsable ? 1 : 0, archiveIsMultipart ? 1 : 0, profile.split_before ? 1 : 0, profile.split_after ? 1 : 0, entry.isSolid ? 1 : 0, profile.has_solid ? 1 : 0, (unsigned int)entry.methodId, entry.hasMethodId ? 1 : 0, supportedAudio ? 1 : 0, entry.streamCandidate ? 1 : 0);

    return true;
}

static bool LaRarBuildEntriesFromHeaderMap(const std::vector<LaRarHeaderCrcInfo>& headerMap, std::vector<ArchiveEntry>& entries)
{
    entries.clear();

    uint32_t index = 0;

    for (const LaRarHeaderCrcInfo& item : headerMap)
    {
        ArchiveEntry entry;
        entry.index = index;
        entry.path = item.path;
        entry.size = item.size;
        entry.isFolder = item.isFolder;
        entry.isSupportedAudio = !entry.isFolder && LaRarIsSupportedAudioPath(entry.path.c_str());
        entry.hasCrc32 = item.hasCrc32;
        entry.crc32 = item.hasCrc32 ? item.crc32 : 0;
        entry.isSolid = item.isSolid;
        entry.folderIndex = 0xFFFFFFFF;
        entry.filesInFolder = 1;
        entry.hasMethodId = item.hasMethodId;
        entry.methodId = item.methodId;
        entry.streamCandidate = entry.isSupportedAudio && !entry.isFolder && !entry.isSolid;

        try
        {
            entries.push_back(entry);
        }
        catch (const std::bad_alloc&)
        {
            entries.clear();
            return false;
        }

        ++index;
    }

    return true;
}

bool LibarchiveRarListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    LibarchiveRarSource source;
    struct archive* a = nullptr;

    if (!LaRarOpenArchive(&a, &source, archivePath, nullptr, error))
    {
        DebugLogWrite("ERROR", "LARAR", "LIST_FAIL", "reason=open_failed | error=%s", error.c_str());
        return false;
    }

    std::vector<LaRarHeaderCrcInfo> headerCrcMap;

    if (source.volumes.size() > 1)
    {
        std::string crcMapError;

        if (!LaRarBuildHeaderCrcMap(source, headerCrcMap, crcMapError))
        {
            DebugLogWrite("WARN", "LARAR", "HEADER_CRC_MAP_UNAVAILABLE", "archive=%s | error=%s", source.pathUtf8.c_str(), crcMapError.c_str());
        }
        else if (LaRarBuildEntriesFromHeaderMap(headerCrcMap, entries))
        {
            archive_read_free(a);
            LaRarCloseSource(&source);
            return true;
        }
    }

    uint32_t index = 0;

    struct archive_entry* entry = nullptr;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            break;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LARAR", "LIST_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", source.pathUtf8.c_str(), (unsigned int)index, error.c_str());
            bool archiveHasSolidEntries = false;

            for (const ArchiveEntry& entryToCheck : entries)
            {
                if (entryToCheck.isSolid)
                {
                    archiveHasSolidEntries = true;
                    break;
                }
            }

            if (archiveHasSolidEntries)
            {
                for (ArchiveEntry& entryToUpdate : entries)
                {
                    entryToUpdate.isSolid = true;
                    entryToUpdate.streamCandidate = false;
                }
            }

            archive_read_free(a);

            LaRarCloseSource(&source);

            return true;
        }

        const char* path = archive_entry_pathname_utf8(entry);

        if (!path)
        {
            path = archive_entry_pathname(entry);
        }

        if (!path)
        {
            path = "";
        }

        bool isFolder = archive_entry_filetype(entry) == AE_IFDIR;
        bool supportedAudio = !isFolder && LaRarIsSupportedAudioPath(path);

        ArchiveEntry out;
        out.index = index;
        out.path = path;
        out.size = archive_entry_size_is_set(entry) ? (uint64_t)archive_entry_size(entry) : 0;
        out.isFolder = isFolder;
        out.isSupportedAudio = supportedAudio;

        if (!LaRarApplyCurrentEntryProfile(a, out, supportedAudio, source.volumes.size() > 1))
        {
            DebugLogWrite("WARN", "LARAR", "ENTRY_PROFILE_UNAVAILABLE", "archive=%s | index=%u | entry=%s | reason=profile_accessor_failed_conservative_fallback", source.pathUtf8.c_str(), (unsigned int)index, path ? path : "");
        }

        try
        {
            entries.push_back(out);
        }
        catch (const std::bad_alloc&)
        {
            error = "Out of memory while listing RAR entries";
            DebugLogWrite("ERROR", "LARAR", "LIST_FAIL", "archive=%s | index=%u | reason=entry_list_alloc_failed", source.pathUtf8.c_str(), (unsigned int)index);
            archive_read_free(a);
            LaRarCloseSource(&source);
            return false;
        }

        if (source.volumes.size() > 1 && supportedAudio)
        {
            ArchiveEntry& listedEntry = entries.back();
            uint32_t headerCrc = 0;

            if (LaRarFindHeaderCrc(headerCrcMap, listedEntry.path, listedEntry.size, headerCrc))
            {
                listedEntry.hasCrc32 = true;
                listedEntry.crc32 = headerCrc;
            }
            else
            {
                listedEntry.hasCrc32 = false;
                listedEntry.crc32 = 0;
                DebugLogWrite("WARN", "LARAR", "HEADER_CRC_MAP_MISS", "archive=%s | index=%u | entry=%s | size=%llu", source.pathUtf8.c_str(), (unsigned int)index, listedEntry.path.c_str(), (unsigned long long)listedEntry.size);
            }

            if (!listedEntry.isSolid)
            {
                int skipResult = archive_read_data_skip(a);

                if (skipResult != ARCHIVE_OK)
                {
                    const char* skipError = archive_error_string(a);
                    DebugLogWrite("WARN", "LARAR", "LIST_SKIP_FAIL", "archive=%s | index=%u | entry=%s | result=%d | error=%s", source.pathUtf8.c_str(), (unsigned int)index, out.path.c_str(), skipResult, skipError ? skipError : "");
                }
            }
        }

        ++index;
    }

    archive_read_free(a);
    LaRarCloseSource(&source);
    return true;
}

static bool LaRarFindEntryByIndex(struct archive** outArchive, LibarchiveRarSource* source, const wchar_t* archivePath, uint32_t targetIndex, std::string& error, const std::vector<std::string>* runtimePasswords = nullptr, std::string* outEntryNameUtf8 = nullptr, bool* outEncrypted = nullptr)

{
    if (outArchive)
    {
        *outArchive = nullptr;
    }

    if (!outArchive || !source)
    {
        error = "Invalid libarchive RAR find-entry inputs";
        return false;
    }

    struct archive* a = nullptr;

    if (!LaRarOpenArchive(&a, source, archivePath, runtimePasswords, error))
    {
        return false;
    }

    struct archive_entry* entry = nullptr;
    uint32_t currentIndex = 0;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            error = "RAR entry index not found";
            DebugLogWrite("ERROR", "LARAR", "FIND_FAIL", "archive=%s | index=%u | reason=index_not_found", source->pathUtf8.c_str(), (unsigned int)targetIndex);
            archive_read_free(a);
            LaRarCloseSource(source);
            return false;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LARAR", "FIND_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", source->pathUtf8.c_str(), (unsigned int)targetIndex, error.c_str());
            archive_read_free(a);
            LaRarCloseSource(source);
            return false;
        }

        if (currentIndex == targetIndex)
        {
            if (archive_entry_filetype(entry) == AE_IFDIR)
            {
                error = "RAR entry is a folder";
                DebugLogWrite("ERROR", "LARAR", "FIND_FAIL", "archive=%s | index=%u | reason=entry_is_folder", source->pathUtf8.c_str(), (unsigned int)targetIndex);
                archive_read_free(a);
                LaRarCloseSource(source);
                return false;
            }

            const char* entryName = archive_entry_pathname_utf8(entry);

            if (!entryName)
            {
                entryName = archive_entry_pathname(entry);
            }

            if (outEntryNameUtf8)
            {
                *outEntryNameUtf8 = entryName ? entryName : "";
            }

            if (outEncrypted)
            {
                *outEncrypted = archive_entry_is_encrypted(entry) > 0;
            }

            if (archive_entry_is_encrypted(entry) > 0)
            {
                DebugLogWrite("WARN", "PASSWORD", "ENCRYPTED_ENTRY", "backend=libarchive_rar | archive=%s | index=%u | entry=%s", source->pathUtf8.c_str(), (unsigned int)targetIndex, entryName ? entryName : "");
            }

            *outArchive = a;
            return true;
        }

        archive_read_data_skip(a);
        ++currentIndex;
    }
}

bool LibarchiveRarExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    data.clear();
    error.clear();

    LibarchiveRarSource source;
    struct archive* a = nullptr;

    if (!LaRarFindEntryByIndex(&a, &source, archivePath, index, error))
    {
        return false;
    }

    unsigned char buffer[256 * 1024];

    for (;;)
    {
        la_ssize_t bytesRead = archive_read_data(a, buffer, sizeof(buffer));

        if (bytesRead == 0)
        {
            break;
        }

        if (bytesRead < 0)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_data failed";
            DebugLogWrite("ERROR", "LARAR", "EXTRACT_MEMORY_FAIL", "archive=%s | index=%u | reason=read_data_failed | error=%s", source.pathUtf8.c_str(), (unsigned int)index, error.c_str());
            archive_read_free(a);
            LaRarCloseSource(&source);
            data.clear();
            return false;
        }

        try
        {
            size_t oldSize = data.size();
            data.resize(oldSize + (size_t)bytesRead);
            memcpy(data.data() + oldSize, buffer, (size_t)bytesRead);
        }
        catch (const std::bad_alloc&)
        {
            error = "Out of memory extracting RAR entry";
            DebugLogWrite("ERROR", "LARAR", "EXTRACT_MEMORY_FAIL", "archive=%s | index=%u | bytes=%zu | reason=alloc_failed", source.pathUtf8.c_str(), (unsigned int)index, data.size());
            archive_read_free(a);
            LaRarCloseSource(&source);
            data.clear();
            return false;
        }
    }

    archive_read_free(a);
    LaRarCloseSource(&source);

    if (data.empty())
    {
        error = "RAR entry extracted to empty buffer";
        DebugLogWrite("ERROR", "LARAR", "EXTRACT_MEMORY_FAIL", "index=%u | reason=empty_output", (unsigned int)index);
        return false;
    }

    return true;
}

static bool LaRarCopyCurrentEntryToFile(struct archive* archiveHandle, const std::string& archivePathUtf8, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    if (!archiveHandle || !outputPath || !outputPath[0])
    {
        error = "Invalid RAR copy-to-file input";
        return false;
    }

    HANDLE output = CreateFileW(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (output == INVALID_HANDLE_VALUE)
    {
        error = "Failed to create RAR output file";
        DebugLogWrite("ERROR", "LARAR", "EXTRACT_FILE_FAIL", "archive=%s | index=%u | reason=create_output_failed | winerr=%lu", archivePathUtf8.c_str(), (unsigned int)index, GetLastError());
        return false;
    }

    unsigned char buffer[256 * 1024];
    bool ok = true;

    for (;;)
    {
        la_ssize_t bytesRead = archive_read_data(archiveHandle, buffer, sizeof(buffer));

        if (bytesRead == 0)
        {
            break;
        }

        if (bytesRead < 0)
        {
            const char* err = archive_error_string(archiveHandle);
            error = err ? err : "archive_read_data failed";
            DebugLogWrite("ERROR", "LARAR", "EXTRACT_FILE_FAIL", "archive=%s | index=%u | reason=read_data_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)index, error.c_str());
            ok = false;
            break;
        }

        DWORD written = 0;

        if (!WriteFile(output, buffer, (DWORD)bytesRead, &written, NULL) || written != (DWORD)bytesRead)
        {
            error = "Failed to write RAR output file";
            DebugLogWrite("ERROR", "LARAR", "EXTRACT_FILE_FAIL", "archive=%s | index=%u | reason=write_failed | winerr=%lu", archivePathUtf8.c_str(), (unsigned int)index, GetLastError());
            ok = false;
            break;
        }
    }

    CloseHandle(output);

    if (!ok)
    {
        DeleteFileW(outputPath);
    }

    return ok;
}

bool LibarchiveRarExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    error.clear();

    if (!archivePath || !archivePath[0])
    {
        error = "Invalid RAR archive path";
        return false;
    }

    if (!outputPath || !outputPath[0])
    {
        error = "Invalid RAR output path";
        return false;
    }

    std::string archivePathUtf8;

    if (!LaRarWcharToUtf8(archivePath, archivePathUtf8))
    {
        error = "Failed to convert RAR path to UTF-8";
        DebugLogWrite("ERROR", "LARAR", "EXTRACT_FILE_FAIL", "index=%u | reason=archive_path_convert_failed", (unsigned int)index);
        return false;
    }

    std::vector<std::string> runtimePasswords;
    bool promptAttempted = false;
    bool pendingSavePassword = false;
    std::string entryNameUtf8;
    bool encrypted = false;

    for (;;)
    {
        LibarchiveRarSource source;
        struct archive* a = nullptr;
        const std::vector<std::string>* runtimePasswordPtr = runtimePasswords.empty() ? nullptr : &runtimePasswords;

        entryNameUtf8.clear();
        encrypted = false;

        if (!LaRarFindEntryByIndex(&a, &source, archivePath, index, error, runtimePasswordPtr, &entryNameUtf8, &encrypted))
        {
            return false;
        }

        bool ok = LaRarCopyCurrentEntryToFile(a, source.pathUtf8, index, outputPath, error);

        archive_read_free(a);
        LaRarCloseSource(&source);

        if (ok)
        {
            if (!runtimePasswords.empty())
            {
                ArchivePasswordClearArchiveCancelled(archivePathUtf8.c_str());
                ArchivePasswordAddRuntimePassword(archivePathUtf8.c_str(), runtimePasswords.back().c_str());
                ArchivePasswordNotifyAccepted(archivePathUtf8.c_str());

                if (pendingSavePassword)
                {
                    std::string saveError;

                    if (!ArchivePasswordAppendIfMissing(runtimePasswords.back().c_str(), saveError))
                    {
                        DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "backend=libarchive_rar | archive=%s | index=%u | error=%s", archivePathUtf8.c_str(), (unsigned int)index, saveError.c_str());
                    }
                }
            }

            return true;
        }

        if (!encrypted)
        {
            return false;
        }

        if (promptAttempted)
        {
            return false;
        }

        if (ArchivePasswordIsArchiveCancelled(archivePathUtf8.c_str()))
        {
            error = "Password prompt already cancelled for archive";
            DebugLogWrite("WARN", "PASSWORD", "EXTRACT_ABORTED_ARCHIVE_CANCELLED", "backend=libarchive_rar | archive=%s | index=%u | entry=%s", archivePathUtf8.c_str(), (unsigned int)index, entryNameUtf8.c_str());
            return false;
        }

        if (!ArchivePasswordGetPromptAllowedForCurrentThread())
        {
            error = "Password prompt suppressed for current thread";
            DebugLogWrite("WARN", "PASSWORD", "EXTRACT_PROMPT_SUPPRESSED", "backend=libarchive_rar | archive=%s | index=%u | entry=%s | reason=prompt_not_allowed", archivePathUtf8.c_str(), (unsigned int)index, entryNameUtf8.c_str());
            return false;
        }

        promptAttempted = true;

        char password[256];
        bool savePassword = false;
        password[0] = '\0';

        DebugLogWrite("WARN", "PASSWORD", "EXTRACT_REQUIRES_PASSWORD", "backend=libarchive_rar | archive=%s | index=%u | entry=%s | error=%s", archivePathUtf8.c_str(), (unsigned int)index, entryNameUtf8.c_str(), error.c_str());

        if (!ArchivePasswordPromptUser(archivePathUtf8.c_str(), entryNameUtf8.c_str(), password, sizeof(password), &savePassword))
        {
            error = "Password prompt cancelled";
            SecureZeroMemory(password, sizeof(password));
            return false;
        }

        if (!password[0])
        {
            error = "Password prompt returned empty password";
            SecureZeroMemory(password, sizeof(password));
            return false;
        }

        runtimePasswords.push_back(password);
        pendingSavePassword = savePassword;

        SecureZeroMemory(password, sizeof(password));
    }
}

bool LibarchiveRarExtractEntriesToFilesWithProgress(
    const wchar_t* archivePath,
    const std::vector<LibarchiveRarExtractFileRequest>& requests,
    std::vector<uint32_t>& succeededIndexes,
    LibarchiveRarExtractProgressCallback progressCallback,
    void* progressUserData,
    std::string& error)
{
    succeededIndexes.clear();
    error.clear();

    if (!archivePath || !archivePath[0])
    {
        error = "Invalid RAR batch archive path";
        return false;
    }

    if (requests.empty())
    {
        return true;
    }

    LibarchiveRarSource source;
    struct archive* a = nullptr;

    if (!LaRarOpenArchive(&a, &source, archivePath, nullptr, error))
    {
        DebugLogWrite("ERROR", "LARAR", "BATCH_EXTRACT_FAIL", "reason=open_failed | error=%s", error.c_str());
        return false;
    }

    DebugLogWrite("INFO", "LARAR", "BATCH_EXTRACT_START", "archive=%s | requests=%zu | progress_callback=%d", source.pathUtf8.c_str(), requests.size(), progressCallback ? 1 : 0);

    uint32_t currentIndex = 0;
    struct archive_entry* entry = nullptr;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            break;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LARAR", "BATCH_EXTRACT_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", source.pathUtf8.c_str(), (unsigned int)currentIndex, error.c_str());
            archive_read_free(a);
            LaRarCloseSource(&source);
            return !succeededIndexes.empty();
        }

        const LibarchiveRarExtractFileRequest* matchedRequest = nullptr;

        for (const LibarchiveRarExtractFileRequest& request : requests)
        {
            if (request.index == currentIndex)
            {
                matchedRequest = &request;
                break;
            }
        }

        if (!matchedRequest)
        {
            archive_read_data_skip(a);
            ++currentIndex;
            continue;
        }

        if (archive_entry_filetype(entry) == AE_IFDIR)
        {
            DebugLogWrite("WARN", "LARAR", "BATCH_EXTRACT_ITEM_SKIP", "archive=%s | index=%u | reason=entry_is_folder", source.pathUtf8.c_str(), (unsigned int)currentIndex);
            archive_read_data_skip(a);
            ++currentIndex;
            continue;
        }

        std::string itemError;

        if (LaRarCopyCurrentEntryToFile(a, source.pathUtf8, currentIndex, matchedRequest->outputPath, itemError))
        {
            succeededIndexes.push_back(currentIndex);
            DebugLogWrite("INFO", "LARAR", "BATCH_EXTRACT_ITEM_DONE", "archive=%s | index=%u", source.pathUtf8.c_str(), (unsigned int)currentIndex);

            if (progressCallback)
            {
                progressCallback(currentIndex, matchedRequest->outputPath, progressUserData);
            }
        }
        else
        {
            DebugLogWrite("WARN", "LARAR", "BATCH_EXTRACT_ITEM_FAIL", "archive=%s | index=%u | error=%s", source.pathUtf8.c_str(), (unsigned int)currentIndex, itemError.c_str());
        }

        ++currentIndex;
    }

    archive_read_free(a);
    LaRarCloseSource(&source);

    DebugLogWrite("INFO", "LARAR", "BATCH_EXTRACT_DONE", "requests=%zu | succeeded=%zu", requests.size(), succeededIndexes.size());

    return !succeededIndexes.empty();
}

bool LibarchiveRarExtractEntriesToFiles(
    const wchar_t* archivePath,
    const std::vector<LibarchiveRarExtractFileRequest>& requests,
    std::vector<uint32_t>& succeededIndexes,
    std::string& error)
{
    return LibarchiveRarExtractEntriesToFilesWithProgress(
        archivePath,
        requests,
        succeededIndexes,
        nullptr,
        nullptr,
        error
    );
}

struct LibarchiveRarEntryStream
{
    std::wstring archivePath;
    std::string archivePathUtf8;
    std::string entryNameUtf8;
    uint32_t index = 0;
    uint64_t position = 0;
    uint64_t entrySize = 0;
    const ArchiveStreamCancellationToken* cancellation = nullptr;
    bool encrypted = false;
    bool promptAttempted = false;
    bool passwordFailureFinal = false;
    bool passwordFailureLogged = false;
    bool pendingPasswordSave = false;
    std::string pendingPasswordToSave;
    std::vector<std::string> runtimePasswords;
    LibarchiveRarSource source;
    struct archive* archiveHandle = nullptr;
};

static void LaRarCloseEntryStreamInternal(LibarchiveRarEntryStream* stream)
{
    if (!stream)
    {
        return;
    }

    if (stream->archiveHandle)
    {
        archive_read_free(stream->archiveHandle);
        stream->archiveHandle = nullptr;
    }

    LaRarCloseSource(&stream->source);
    stream->position = 0;
}

static bool LaRarOpenEntryStreamInternal(LibarchiveRarEntryStream* stream, std::string& error)
{
    if (!stream)
    {
        error = "Invalid libarchive RAR stream";
        return false;
    }

    LaRarCloseEntryStreamInternal(stream);

    struct archive* a = nullptr;

    if (!LaRarOpenArchive(&a, &stream->source, stream->archivePath.c_str(), &stream->runtimePasswords, error))
    {
        DebugLogWrite("ERROR", "LARAR", "STREAM_OPEN_FAIL", "index=%u | reason=open_failed | error=%s", (unsigned int)stream->index, error.c_str());
        return false;
    }

    stream->archivePathUtf8 = stream->source.pathUtf8;

    struct archive_entry* entry = nullptr;
    uint32_t currentIndex = 0;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            error = "RAR stream entry index not found";
            DebugLogWrite("ERROR", "LARAR", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=index_not_found", stream->archivePathUtf8.c_str(), (unsigned int)stream->index);
            archive_read_free(a);
            stream->archiveHandle = nullptr;
            LaRarCloseSource(&stream->source);
            return false;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LARAR", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, error.c_str());
            archive_read_free(a);
            stream->archiveHandle = nullptr;
            LaRarCloseSource(&stream->source);
            return false;
        }

        if (currentIndex == stream->index)
        {
            if (archive_entry_filetype(entry) == AE_IFDIR)
            {
                error = "RAR stream entry is a folder";
                DebugLogWrite("ERROR", "LARAR", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=entry_is_folder", stream->archivePathUtf8.c_str(), (unsigned int)stream->index);
                archive_read_free(a);
                stream->archiveHandle = nullptr;
                LaRarCloseSource(&stream->source);
                return false;
            }

            const char* entryName = archive_entry_pathname_utf8(entry);

            if (!entryName)
            {
                entryName = archive_entry_pathname(entry);
            }

            stream->entryNameUtf8 = entryName ? entryName : "";
            stream->encrypted = archive_entry_is_encrypted(entry) > 0;

            if (stream->encrypted)
            {
                DebugLogWrite("WARN", "PASSWORD", "ENCRYPTED_ENTRY", "backend=libarchive_rar | archive=%s | index=%u | entry=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
            }

            stream->archiveHandle = a;
            stream->position = 0;
            stream->entrySize = archive_entry_size_is_set(entry) ? (uint64_t)archive_entry_size(entry) : 0;
            return true;
        }

        archive_read_data_skip(a);
        ++currentIndex;
    }
}

bool LibarchiveRarOpenEntryStream(const wchar_t* archivePath, uint32_t index, void** outStream, uint64_t* outEntrySize, std::string& error, const ArchiveStreamCancellationToken* cancellation)
{
    if (outStream)
    {
        *outStream = nullptr;
    }

    if (outEntrySize)
    {
        *outEntrySize = 0;
    }

    error.clear();

    if (!archivePath || !archivePath[0] || !outStream)
    {
        error = "Invalid RAR stream open input";
        return false;
    }

    LibarchiveRarEntryStream* stream = new (std::nothrow) LibarchiveRarEntryStream();

    if (!stream)
    {
        error = "Failed to allocate RAR stream";
        DebugLogWrite("ERROR", "LARAR", "STREAM_OPEN_FAIL", "index=%u | reason=stream_alloc_failed", (unsigned int)index);
        return false;
    }

    stream->archivePath = archivePath;
    stream->index = index;
    stream->cancellation = cancellation;

    if (!LaRarOpenEntryStreamInternal(stream, error))
    {
        delete stream;
        return false;
    }

    if (outEntrySize)
    {
        *outEntrySize = stream->entrySize;
    }

    *outStream = stream;
    return true;
}

static bool LaRarPromptAndReopenEncryptedStream(LibarchiveRarEntryStream* stream, std::string& error)
{
    if (!stream)
    {
        error = "Invalid RAR password prompt stream";
        return false;
    }

    if (ArchivePasswordIsArchiveCancelled(stream->archivePathUtf8.c_str()))
    {
        error = "Password prompt already cancelled for archive";
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SUPPRESSED", "backend=libarchive_rar | archive=%s | index=%u | reason=archive_cancelled", stream->archivePathUtf8.c_str(), (unsigned int)stream->index);
        return false;
    }

    if (stream->promptAttempted)
    {
        error = "Password prompt already attempted";
        return false;
    }

    stream->promptAttempted = true;

    char password[256];
    bool savePassword = false;

    password[0] = '\0';

    if (!ArchivePasswordPromptUser(stream->archivePathUtf8.c_str(), stream->entryNameUtf8.c_str(), password, sizeof(password), &savePassword))
    {
        error = "Password prompt cancelled";
        return false;
    }

    if (!password[0])
    {
        error = "Password prompt returned empty password";
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_EMPTY", "backend=libarchive_rar | archive=%s | index=%u | entry=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
        return false;
    }

    stream->runtimePasswords.push_back(password);
    stream->pendingPasswordSave = savePassword;
    stream->pendingPasswordToSave = password;

    if (!LaRarOpenEntryStreamInternal(stream, error))
    {
        DebugLogWrite("ERROR", "PASSWORD", "REOPEN_FAIL", "backend=libarchive_rar | archive=%s | index=%u | entry=%s | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str(), error.c_str());
        SecureZeroMemory(password, sizeof(password));
        return false;
    }

    SecureZeroMemory(password, sizeof(password));
    return true;
}

size_t LibarchiveRarReadEntryStream(void* streamHandle, void* buffer, size_t bytesToRead, std::string& error)
{
    error.clear();

    LibarchiveRarEntryStream* stream = static_cast<LibarchiveRarEntryStream*>(streamHandle);

    if (!stream || !stream->archiveHandle || !buffer || bytesToRead == 0)
    {
        return 0;
    }

    if (stream->cancellation && stream->cancellation->IsCancellationRequested())
    {
        error = "RAR stream cancelled";
        return 0;
    }

    if (stream->passwordFailureFinal)
    {
        return 0;
    }

    if (stream->encrypted && ArchivePasswordIsArchiveCancelled(stream->archivePathUtf8.c_str()))
    {
        stream->passwordFailureFinal = true;

        if (!stream->passwordFailureLogged)
        {
            stream->passwordFailureLogged = true;
            DebugLogWrite("WARN", "PASSWORD", "READ_ABORTED_ARCHIVE_CANCELLED", "backend=libarchive_rar | archive=%s | index=%u | entry=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
        }

        return 0;
    }

    la_ssize_t bytesRead = archive_read_data(stream->archiveHandle, buffer, bytesToRead);

    if (bytesRead < 0 && stream->encrypted && stream->position == 0 && !stream->promptAttempted)
    {
        const char* firstErr = archive_error_string(stream->archiveHandle);

        if (!stream->passwordFailureLogged)
        {
            stream->passwordFailureLogged = true;
            DebugLogWrite("WARN", "PASSWORD", "READ_REQUIRES_PASSWORD", "backend=libarchive_rar | archive=%s | index=%u | entry=%s | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str(), firstErr ? firstErr : "archive_read_data_failed");
        }

        if (LaRarPromptAndReopenEncryptedStream(stream, error))
        {
            bytesRead = archive_read_data(stream->archiveHandle, buffer, bytesToRead);
        }
        else
        {
            stream->passwordFailureFinal = true;
            return 0;
        }
    }

    if (bytesRead < 0)
    {
        const char* err = archive_error_string(stream->archiveHandle);
        error = err ? err : "archive_read_data failed";

        if (!stream->passwordFailureLogged)
        {
            stream->passwordFailureLogged = true;
            DebugLogWrite("ERROR", "LARAR", "STREAM_READ_FAIL", "index=%u | position=%llu | requested=%zu | reason=read_data_failed | error=%s", (unsigned int)stream->index, (unsigned long long)stream->position, bytesToRead, error.c_str());
        }

        stream->passwordFailureFinal = true;
        return 0;
    }

    if (bytesRead > 0 && !stream->pendingPasswordToSave.empty())
    {
        ArchivePasswordClearArchiveCancelled(stream->archivePathUtf8.c_str());
        ArchivePasswordAddRuntimePassword(stream->archivePathUtf8.c_str(), stream->pendingPasswordToSave.c_str());
        ArchivePasswordNotifyAccepted(stream->archivePathUtf8.c_str());

        if (stream->pendingPasswordSave)
        {
            std::string saveError;

            if (!ArchivePasswordAppendIfMissing(stream->pendingPasswordToSave.c_str(), saveError))
            {
                DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "backend=libarchive_rar | archive=%s | index=%u | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, saveError.c_str());
            }
        }

        stream->pendingPasswordSave = false;
        SecureZeroMemory(&stream->pendingPasswordToSave[0], stream->pendingPasswordToSave.size());
        stream->pendingPasswordToSave.clear();
    }

    stream->position += (uint64_t)bytesRead;
    return (size_t)bytesRead;
}

bool LibarchiveRarSeekEntryStream(void* streamHandle, uint64_t absolutePosition, std::string& error)
{
    error.clear();

    LibarchiveRarEntryStream* stream = static_cast<LibarchiveRarEntryStream*>(streamHandle);

    if (!stream || !stream->archiveHandle)
    {
        error = "Invalid RAR stream seek input";
        return false;
    }

    if (stream->cancellation && stream->cancellation->IsCancellationRequested())
    {
        error = "RAR seek cancelled";
        return false;
    }

    if (absolutePosition == stream->position)
    {
        return true;
    }

    if (absolutePosition < stream->position)
    {
        if (!LaRarOpenEntryStreamInternal(stream, error))
        {
            return false;
        }
    }

    unsigned char discard[64 * 1024];

    while (stream->position < absolutePosition)
    {
        if (stream->cancellation && stream->cancellation->IsCancellationRequested())
        {
            error = "RAR seek cancelled";
            return false;
        }

        uint64_t remaining = absolutePosition - stream->position;
        size_t request = (size_t)std::min<uint64_t>(remaining, sizeof(discard));
        size_t got = LibarchiveRarReadEntryStream(stream, discard, request, error);

        if (got == 0)
        {
            if (error.empty())
            {
                error = "RAR seek reached EOF before target";
            }

            DebugLogWrite("ERROR", "LARAR", "STREAM_SEEK_FAIL", "index=%u | current=%llu | target=%llu | reason=read_discard_failed | error=%s", (unsigned int)stream->index, (unsigned long long)stream->position, (unsigned long long)absolutePosition, error.c_str());
            return false;
        }
    }

    return true;
}

void LibarchiveRarCloseEntryStream(void* streamHandle)
{
    LibarchiveRarEntryStream* stream = static_cast<LibarchiveRarEntryStream*>(streamHandle);

    if (!stream)
    {
        return;
    }

    LaRarCloseEntryStreamInternal(stream);

    if (!stream->pendingPasswordToSave.empty())
    {
        SecureZeroMemory(&stream->pendingPasswordToSave[0], stream->pendingPasswordToSave.size());
        stream->pendingPasswordToSave.clear();
    }

    delete stream;
}