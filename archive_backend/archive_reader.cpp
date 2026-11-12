#include <windows.h>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>

#include "archive_reader.h"
#include "archive_common.h"
#include "archive_cache.h"
#include "archive_backend.h"
#include "archive_types.h"
#include "debug_log.h"
#include "archive_metadata.h"
#include "archive_zip_libarchive.h"

static INIT_ONCE g_archiveReaderRarLockInitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_archiveReaderRarLock;

static BOOL CALLBACK ArchiveReaderInitRarLock(PINIT_ONCE initOnce, PVOID parameter, PVOID* context)
{
    (void)initOnce;
    (void)parameter;
    (void)context;

    InitializeCriticalSection(&g_archiveReaderRarLock);
    return TRUE;
}

static void ArchiveReaderEnterRarLock()
{
    InitOnceExecuteOnce(&g_archiveReaderRarLockInitOnce, ArchiveReaderInitRarLock, NULL, NULL);
    EnterCriticalSection(&g_archiveReaderRarLock);
}

static void ArchiveReaderLeaveRarLock()
{
    LeaveCriticalSection(&g_archiveReaderRarLock);
}

static bool ArchiveReaderPathIsRar(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return false;
    }

    const char* ext = strrchr(archivePath, '.');

    if (!ext)
    {
        return false;
    }

    return _stricmp(ext, ".rar") == 0;
}

struct ArchiveReaderRarListCache
{
    bool valid = false;
    std::wstring path;
    ULONGLONG fileSize = 0;
    FILETIME lastWriteTime = {};
    std::vector<ArchiveEntry> entries;
};

static ArchiveReaderRarListCache g_archiveReaderRarListCache;

static ULONGLONG ArchiveReaderMakeFileSize64(const WIN32_FILE_ATTRIBUTE_DATA& data)
{
    return (((ULONGLONG)data.nFileSizeHigh) << 32) | (ULONGLONG)data.nFileSizeLow;
}

static bool ArchiveReaderRarCacheMatchesLocked(const wchar_t* widePath, const WIN32_FILE_ATTRIBUTE_DATA& fileData)
{
    if (!g_archiveReaderRarListCache.valid || !widePath || !widePath[0])
    {
        return false;
    }

    if (lstrcmpiW(g_archiveReaderRarListCache.path.c_str(), widePath) != 0)
    {
        return false;
    }

    if (g_archiveReaderRarListCache.fileSize != ArchiveReaderMakeFileSize64(fileData))
    {
        return false;
    }

    return CompareFileTime(&g_archiveReaderRarListCache.lastWriteTime, &fileData.ftLastWriteTime) == 0;
}

static bool ArchiveReaderListEntriesLockedIfRar(const char* archivePath, const wchar_t* widePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    if (!ArchiveReaderPathIsRar(archivePath))
    {
        return ArchiveListEntries(widePath, entries, error);
    }

    WIN32_FILE_ATTRIBUTE_DATA fileData = {};
    bool haveFileData = GetFileAttributesExW(widePath, GetFileExInfoStandard, &fileData) != FALSE;

    ArchiveReaderEnterRarLock();

    if (haveFileData && ArchiveReaderRarCacheMatchesLocked(widePath, fileData))
    {
        entries = g_archiveReaderRarListCache.entries;
        ArchiveReaderLeaveRarLock();
        //DebugLogWrite("INFO", "SCAN", "RAR_LIST_CACHE_HIT", "archive=%s | entries=%zu", archivePath ? archivePath : "", entries.size());
        return true;
    }

    std::vector<ArchiveEntry> freshEntries;
    bool ok = ArchiveListEntries(widePath, freshEntries, error);

    if (ok && haveFileData)
    {
        g_archiveReaderRarListCache.valid = true;
        g_archiveReaderRarListCache.path = widePath;
        g_archiveReaderRarListCache.fileSize = ArchiveReaderMakeFileSize64(fileData);
        g_archiveReaderRarListCache.lastWriteTime = fileData.ftLastWriteTime;
        g_archiveReaderRarListCache.entries = freshEntries;
        DebugLogWrite("INFO", "SCAN", "RAR_LIST_CACHE_STORE", "archive=%s | entries=%zu", archivePath ? archivePath : "", freshEntries.size());
    }

    ArchiveReaderLeaveRarLock();

    if (ok)
    {
        entries = freshEntries;
    }

    return ok;
}

static bool ArchiveReaderExtractEntryToMemoryLockedIfRar(const char* archivePath, const wchar_t* widePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    if (ArchiveReaderPathIsRar(archivePath))
    {
        ArchiveReaderEnterRarLock();
        bool ok = ArchiveExtractEntryToMemory(widePath, index, data, error);
        ArchiveReaderLeaveRarLock();
        return ok;
    }

    return ArchiveExtractEntryToMemory(widePath, index, data, error);
}

static bool ArchiveReaderOpenEntryStreamLockedIfRar(const char* archivePath, const wchar_t* widePath, uint32_t index, ArchiveBackendStream& backendStream, std::string& error, bool* outLockHeld)
{
    (void)archivePath;

    if (outLockHeld)
    {
        *outLockHeld = false;
    }

    return ArchiveOpenEntryStream(widePath, index, backendStream, error);
}

bool CharPathToWidePath(const char* path, wchar_t* outWidePath, size_t outWidePathCount);



bool ArchiveGetEntryInfoEx(const char* archivePath, mz_uint index, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize, mz_uint32* outCrc32, bool* outHasCrc32)
{
    if (outName && outNameSize > 0)
        outName[0] = '\0';

    if (outUncompressedSize)
        *outUncompressedSize = 0;

    if (outCrc32)
        *outCrc32 = 0;

    if (outHasCrc32)
        *outHasCrc32 = false;

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=empty_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    /*
    The old direct miniz ZIP fast path has been removed.

    ZIP entry info now falls through to the shared ArchiveListEntries() path,
    which routes ZIP listing through libarchive. Keeping this branch disabled
    avoids maintaining a second ZIP metadata path.
*/


    if (IsZipArchivePath(archivePath) || IsSevenZipArchivePath(archivePath) || IsRarArchivePath(archivePath))
    {
        wchar_t widePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
        {
            DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
            return false;
        }

        std::vector<ArchiveEntry> entries;
        std::string error;

        if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, entries, error))
        {
            DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=list_entries_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        for (const ArchiveEntry& e : entries)
        {
            if (e.index != index)
                continue;

            if (e.isFolder || !e.isSupportedAudio)
            {
                DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=not_supported_audio_or_folder", archivePath, (unsigned int)index);
                return false;
            }

            if (outName && outNameSize > 0)
                strcpy_s(outName, outNameSize, e.path.c_str());

            if (outUncompressedSize)
                *outUncompressedSize = (mz_uint64)e.size;

            if (outCrc32)
                *outCrc32 = (mz_uint32)e.crc32;

            if (outHasCrc32)
                *outHasCrc32 = e.hasCrc32;

            return true;
        }

        DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePath, (unsigned int)index);
        return false;
    }

    DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_EX_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
    return false;
}


//--------------------


static const char* ArchiveTypeFromPath(const char* archivePath)
{
    if (IsZipArchivePath(archivePath))
        return "ZIP";

    if (IsSevenZipArchivePath(archivePath))
        return "7Z";

    if (IsRarArchivePath(archivePath))
        return "RAR";

    return "UNKNOWN";
}

static const char* AudioTypeFromPath(const char* internalName)
{
    AudioFormat format = GetAudioFormatFromPath(internalName);

    switch (format)
    {
    case AUDIO_FORMAT_MP3: return "MP3";
    case AUDIO_FORMAT_WAV: return "WAV";
    case AUDIO_FORMAT_FLAC: return "FLAC";
    case AUDIO_FORMAT_OGG: return "OGG";
    case AUDIO_FORMAT_M4A: return "M4A";
    case AUDIO_FORMAT_AAC: return "AAC";
    case AUDIO_FORMAT_WMA: return "WMA";
    default: return "UNKNOWN";
    }
}

static const char* BaseNameNoExtensionFromPath(const char* path, char* out, size_t outSize)
{
    if (!out || outSize == 0)
        return "";

    out[0] = '\0';

    if (!path || !path[0])
        return out;

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');
    const char* base = path;

    if (slash1 && slash1 + 1 > base)
        base = slash1 + 1;

    if (slash2 && slash2 + 1 > base)
        base = slash2 + 1;

    strcpy_s(out, outSize, base);
    char* ext = strrchr(out, '.');

    if (ext)
        *ext = '\0';

    return out;
}

static bool ParseVirtualArchiveDisplayParts(const char* virtualPath, char* archivePath, size_t archivePathSize, unsigned int* outIndexOneBased, char* internalPath, size_t internalPathSize)
{
    if (archivePath && archivePathSize > 0)
        archivePath[0] = '\0';

    if (internalPath && internalPathSize > 0)
        internalPath[0] = '\0';

    if (outIndexOneBased)
        *outIndexOneBased = 0;

    if (!virtualPath || !virtualPath[0])
        return false;

    const char* path = virtualPath;

    if (strncmp(path, VIRTUAL_PREFIX, strlen(VIRTUAL_PREFIX)) == 0)
        path += strlen(VIRTUAL_PREFIX);

    const char* marker = strstr(path, ".archive_stream|");

    if (!marker)
        return false;

    size_t archiveLen = (size_t)(marker - path);

    if (!archivePath || archivePathSize == 0 || archiveLen >= archivePathSize)
        return false;

    memcpy(archivePath, path, archiveLen);
    archivePath[archiveLen] = '\0';
    const char* indexStart = marker + strlen(".archive_stream|");
    const char* pipe = strchr(indexStart, '|');

    if (!pipe)
        return false;

    char indexText[32];
    size_t indexLen = (size_t)(pipe - indexStart);

    if (indexLen >= sizeof(indexText))
        indexLen = sizeof(indexText) - 1;

    memcpy(indexText, indexStart, indexLen);
    indexText[indexLen] = '\0';
    unsigned int zeroBasedIndex = (unsigned int)atoi(indexText);

    if (outIndexOneBased)
        *outIndexOneBased = zeroBasedIndex + 1;

    const char* internal = pipe + 1;

    if (internalPath && internalPathSize > 0)
        strcpy_s(internalPath, internalPathSize, internal ? internal : "");

    return true;
}

static void ReplaceAllToken(std::string& text, const char* token, const char* value)
{
    if (!token || !token[0])
        return;

    const char* safeValue = value ? value : "";
    size_t pos = 0;
    size_t tokenLen = strlen(token);

    while ((pos = text.find(token, pos)) != std::string::npos)
    {
        text.replace(pos, tokenLen, safeValue);
        pos += strlen(safeValue);
    }
}


ArchiveStreamCapability ArchiveGetEntryStreamCapability(const char* archivePath, mz_uint index)
{
    if (!archivePath || !archivePath[0])
    {
        return ARCHIVE_STREAM_CAPABILITY_NONE;
    }

    if (IsZipArchivePath(archivePath))
    {
        DebugLogWrite("INFO", "STREAM", "CAPABILITY_VERIFIED", "archive=%s | index=%u | type=ZIP | result=supported | reason=libarchive_zip_stream_path", archivePath, (unsigned int)index);
        return ARCHIVE_STREAM_CAPABILITY_SUPPORTED;
    }

    if (IsSevenZipArchivePath(archivePath) || IsRarArchivePath(archivePath))
    {
        bool isSolid = false;
        mz_uint32 folderIndex = 0xFFFFFFFF;
        mz_uint32 filesInFolder = 0;
        const char* archiveType = IsSevenZipArchivePath(archivePath) ? "7Z" : "RAR";

        if (!ArchiveGetEntrySolidInfo(archivePath, index, &isSolid, &folderIndex, &filesInFolder))
        {
            DebugLogWrite("WARN", "STREAM", "CAPABILITY_UNKNOWN", "archive=%s | index=%u | type=%s | result=not_supported | reason=solid_info_unavailable_no_assumption", archivePath, (unsigned int)index, archiveType);
            return ARCHIVE_STREAM_CAPABILITY_NONE;
        }

        if (isSolid)
        {
            DebugLogWrite("INFO", "STREAM", "CAPABILITY_VERIFIED", "archive=%s | index=%u | type=%s | result=not_supported | reason=solid_entry | folder_index=%u | files_in_folder=%u", archivePath, (unsigned int)index, archiveType, (unsigned int)folderIndex, (unsigned int)filesInFolder);
            return ARCHIVE_STREAM_CAPABILITY_NONE;
        }

        DebugLogWrite("INFO", "STREAM", "CAPABILITY_VERIFIED", "archive=%s | index=%u | type=%s | result=supported | reason=non_solid_entry | folder_index=%u | files_in_folder=%u", archivePath, (unsigned int)index, archiveType, (unsigned int)folderIndex, (unsigned int)filesInFolder);
        return ARCHIVE_STREAM_CAPABILITY_SUPPORTED;
    }

    DebugLogWrite("INFO", "STREAM", "CAPABILITY_VERIFIED", "archive=%s | index=%u | type=UNKNOWN | result=not_supported | reason=unsupported_archive_type", archivePath, (unsigned int)index);
    return ARCHIVE_STREAM_CAPABILITY_NONE;
}

bool ArchiveEntryCanStream(const char* archivePath, mz_uint index)
{
    return ArchiveGetEntryStreamCapability(archivePath, index) == ARCHIVE_STREAM_CAPABILITY_SUPPORTED;
}

bool CharPathToWidePath(const char* path, wchar_t* outWidePath, size_t outWidePathCount)
{
    if (!path || !path[0] || !outWidePath || outWidePathCount == 0)
        return false;

    outWidePath[0] = L'\0';
    int chars = MultiByteToWideChar(CP_UTF8, 0, path, -1, outWidePath, (int)outWidePathCount);

    if (chars > 0)
        return true;

    chars = MultiByteToWideChar(CP_ACP, 0, path, -1, outWidePath, (int)outWidePathCount);

    return chars > 0;
}

// -----------------------------------------------------------------------------
// IsSevenZipArchivePath
//
// Returns true for normal 7z archives and first-volume multipart 7z archives.
//
// Supported:
//     file.7z
//     file.7z.001
//
// Not supported as direct playlist archive inputs:
//     file.7z.002
//     file.001 without .7z before it
//
// This avoids claiming unrelated split files while allowing Winamp to expand and
// play the first part of a split 7z set.
// -----------------------------------------------------------------------------
bool IsSevenZipArchivePath(const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    size_t len = strlen(path);

    if (len >= 3 && _stricmp(path + len - 3, ".7z") == 0)
    {
        return true;
    }

    if (len >= 7 && _stricmp(path + len - 7, ".7z.001") == 0)
    {
        return true;
    }

    return false;
}

bool IsRarArchivePath(const char* path)
{
    if (!path || !path[0])
        return false;

    const char* ext = strrchr(path, '.');

    if (!ext)
        return false;

    return _stricmp(ext, ".rar") == 0;
}

// -----------------------------------------------------------------------------
// IsZipArchivePath
//
// Returns true for normal ZIP archives and first-volume split ZIP archives.
//
// Supported:
//     file.zip
//     file.zip.001
//
// Not accepted:
//     file.zip.002
//     file.001 without .zip before it
//
// This lets Winamp route split ZIP first volumes into the archive plugin without
// claiming unrelated .001 files.
// -----------------------------------------------------------------------------
bool IsZipArchivePath(const char* path)
{
    if (!path || !path[0])
    {
        return false;
    }

    size_t len = strlen(path);

    if (len >= 4 && _stricmp(path + len - 4, ".zip") == 0)
    {
        return true;
    }

    if (len >= 8 && _stricmp(path + len - 8, ".zip.001") == 0)
    {
        return true;
    }

    return false;
}

bool ArchiveIsSevenZipPath(const char* archivePath)
{
    return IsSevenZipArchivePath(archivePath);
}

bool ArchiveIsZipPath(const char* archivePath)
{
    return IsZipArchivePath(archivePath);
}

bool IsSupportedArchivePath(const char* path)
{
    return IsZipArchivePath(path) || IsSevenZipArchivePath(path) || IsRarArchivePath(path);
}

bool ArchiveListAudioEntries(const char* archivePath, std::vector<ArchiveAudioEntry>& outEntries)
{
    outEntries.clear();

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "SCAN", "AUDIO_SCAN_FAIL", "archive=%s | reason=empty_path", archivePath ? archivePath : "");
        return false;
    }

    if (!IsSupportedArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "SCAN", "AUDIO_SCAN_FAIL", "archive=%s | reason=unsupported_archive", archivePath);
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        DebugLogWrite("ERROR", "SCAN", "AUDIO_SCAN_FAIL", "archive=%s | reason=path_convert_failed", archivePath);
        return false;
    }

    std::vector<ArchiveEntry> backendEntries;
    std::string error;

    if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, backendEntries, error))
    {
        DebugLogWrite("ERROR", "SCAN", "AUDIO_SCAN_FAIL", "archive=%s | reason=list_entries_failed | error=%s", archivePath, error.c_str());
        return false;
    }

    for (const ArchiveEntry& backendEntry : backendEntries)
    {
        if (backendEntry.isFolder)
            continue;

        if (!backendEntry.isSupportedAudio)
            continue;

        ArchiveAudioEntry entry;
        entry.index = backendEntry.index;
        entry.uncompSize = backendEntry.size;
        entry.name = backendEntry.path;

        outEntries.push_back(entry);
    }

    std::sort(outEntries.begin(), outEntries.end(), [](const ArchiveAudioEntry& a, const ArchiveAudioEntry& b)
        {
            return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
        }
    );

    if (outEntries.empty())
    {
        DebugLogWrite("WARN", "SCAN", "AUDIO_SCAN_FAIL", "archive=%s | backend_entries=%zu | reason1=no_supported_audio_entries", archivePath, backendEntries.size());
        return false;
    }

    return true;
}

bool ArchiveFindFirstAudioEntry(const char* archivePath, mz_uint* outIndex, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize)
{
    if (outIndex)
        *outIndex = 0;

    if (outName && outNameSize > 0)
        outName[0] = '\0';

    if (outUncompressedSize)
        *outUncompressedSize = 0;

    std::vector<ArchiveAudioEntry> entries;

    if (!ArchiveListAudioEntries(archivePath, entries))
    {
        DebugLogWrite("ERROR", "SCAN", "FIND_FIRST_AUDIO_FAIL", "archive=%s | reason=audio_scan_failed", archivePath ? archivePath : "");
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "SCAN", "FIND_FIRST_AUDIO_FAIL", "archive=%s | reason=no_audio_entries", archivePath ? archivePath : "");
        return false;
    }

    const ArchiveAudioEntry& first = entries[0];

    if (outIndex)
        *outIndex = first.index;

    if (outName && outNameSize > 0)
        strcpy_s(outName, outNameSize, first.name.c_str());

    if (outUncompressedSize)
        *outUncompressedSize = first.uncompSize;

    return true;
}

// -----------------------------------------------------------------------------
// ArchiveGetEntryInfo
//
// Returns display/playback information for one playable archive entry.
//
// This deliberately uses the shared backend listing path for all archive types,
// including ZIP. Split ZIP first volumes such as ".zip.001" need preprocessing
// before miniz can read them, and ZipListEntries() already handles that by joining
// the split parts into a temporary complete ZIP. Opening ZIP files directly here
// would bypass that split ZIP support and fail with zip_reader_init_failed.
//
// All hard failures are logged because this function blocks virtual-track
// playback if it cannot resolve the selected entry.
// -----------------------------------------------------------------------------
bool ArchiveGetEntryInfo(const char* archivePath, mz_uint index, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize)
{
    if (outName && outNameSize > 0)
    {
        outName[0] = '\0';
    }

    if (outUncompressedSize)
    {
        *outUncompressedSize = 0;
    }

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=empty_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    if (!IsZipArchivePath(archivePath) && !IsSevenZipArchivePath(archivePath) && !IsRarArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
        return false;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, entries, error))
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=list_entries_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
        return false;
    }

    for (const ArchiveEntry& e : entries)
    {
        if (e.index != index)
        {
            continue;
        }

        if (e.isFolder || !e.isSupportedAudio)
        {
            DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=not_supported_audio_or_folder", archivePath, (unsigned int)index);
            return false;
        }

        if (outName && outNameSize > 0)
        {
            strcpy_s(outName, outNameSize, e.path.c_str());
        }

        if (outUncompressedSize)
        {
            *outUncompressedSize = e.size;
        }

        return true;
    }

    DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePath, (unsigned int)index);
    return false;
}

static bool ArchiveInternalPathEquals(const char* leftPath, const char* rightPath)
{
    if (!leftPath || !rightPath)
    {
        return false;
    }

    char left[1024];
    char right[1024];

    strcpy_s(left, sizeof(left), leftPath);
    strcpy_s(right, sizeof(right), rightPath);

    for (char* p = left; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\\';
        }
    }

    for (char* p = right; *p; ++p)
    {
        if (*p == '/')
        {
            *p = '\\';
        }
    }

    return _stricmp(left, right) == 0;
}

// -----------------------------------------------------------------------------
// ArchiveGetEntryInfoByPath
//
// Resolves a virtual entry by internal archive path.
//
// This is the fallback path when the virtual index lookup fails. It uses the
// shared backend listing path for every archive type, including ZIP, so split ZIP
// first volumes go through the same temporary-join support used during playlist
// expansion. Direct ZIP miniz opens are intentionally avoided here.
// -----------------------------------------------------------------------------
bool ArchiveGetEntryInfoByPath(const char* archivePath, const char* internalPath, char* outName, size_t outNameSize, uint64_t* outUncompressedSize, uint32_t* outIndex)
{
    if (outName && outNameSize > 0)
    {
        outName[0] = '\0';
    }

    if (outUncompressedSize)
    {
        *outUncompressedSize = 0;
    }

    if (outIndex)
    {
        *outIndex = 0;
    }

    if (!archivePath || !archivePath[0] || !internalPath || !internalPath[0])
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_BY_PATH_FAIL", "archive=%s | internal=%s | reason=invalid_input", archivePath ? archivePath : "", internalPath ? internalPath : "");
        return false;
    }

    if (!IsZipArchivePath(archivePath) && !IsSevenZipArchivePath(archivePath) && !IsRarArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_BY_PATH_FAIL", "archive=%s | internal=%s | reason=unsupported_archive", archivePath, internalPath);
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_BY_PATH_FAIL", "archive=%s | internal=%s | reason=path_convert_failed", archivePath, internalPath);
        return false;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, entries, error))
    {
        DebugLogWrite("ERROR", "SCAN", "ENTRY_INFO_BY_PATH_FAIL", "archive=%s | internal=%s | reason=list_entries_failed | error=%s", archivePath, internalPath, error.c_str());
        return false;
    }

    for (const ArchiveEntry& entry : entries)
    {
        if (entry.isFolder || !entry.isSupportedAudio)
        {
            continue;
        }

        if (!ArchiveInternalPathEquals(entry.path.c_str(), internalPath))
        {
            continue;
        }

        if (outName && outNameSize > 0)
        {
            strcpy_s(outName, outNameSize, entry.path.c_str());
        }

        if (outUncompressedSize)
        {
            *outUncompressedSize = entry.size;
        }

        if (outIndex)
        {
            *outIndex = entry.index;
        }

        DebugLogWrite("INFO", "SCAN", "ENTRY_INFO_BY_PATH_OK", "archive=%s | internal=%s | index=%u", archivePath, internalPath, (unsigned int)entry.index);
        return true;
    }

    DebugLogWrite("WARN", "SCAN", "ENTRY_INFO_BY_PATH_FAIL", "archive=%s | internal=%s | reason=path_not_found", archivePath, internalPath);
    return false;
}



bool ArchiveGetEntrySolidInfo(const char* archivePath, mz_uint index, bool* outIsSolid, mz_uint32* outFolderIndex, mz_uint32* outFilesInFolder)
{
    if (outIsSolid)
        *outIsSolid = false;

    if (outFolderIndex)
        *outFolderIndex = 0xFFFFFFFF;

    if (outFilesInFolder)
        *outFilesInFolder = 0;

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("WARN", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=empty_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    if (IsZipArchivePath(archivePath))
    {
        if (outIsSolid)
            *outIsSolid = false;

        if (outFolderIndex)
            *outFolderIndex = 0xFFFFFFFF;

        if (outFilesInFolder)
            *outFilesInFolder = 1;

        return true;
    }

    if (IsRarArchivePath(archivePath))
    {
        wchar_t widePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
        {
            DebugLogWrite("ERROR", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
            return false;
        }

        std::vector<ArchiveEntry> entries;
        std::string error;

        if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, entries, error))
        {
            DebugLogWrite("ERROR", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=list_entries_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        for (const ArchiveEntry& e : entries)
        {
            if (e.index != index)
                continue;

            if (outIsSolid)
                *outIsSolid = e.isSolid;

            if (outFolderIndex)
                *outFolderIndex = 0xFFFFFFFF;

            if (outFilesInFolder)
                *outFilesInFolder = 0;

            return true;
        }

        DebugLogWrite("WARN", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePath, (unsigned int)index);
        return false;
    }

    if (!IsSevenZipArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        DebugLogWrite("ERROR", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
        return false;
    }

    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!ArchiveReaderListEntriesLockedIfRar(archivePath, widePath, entries, error))
    {
        DebugLogWrite("ERROR", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=list_entries_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
        return false;
    }

    for (const ArchiveEntry& e : entries)
    {
        if (e.index != index)
            continue;

        if (outIsSolid)
            *outIsSolid = e.isSolid;

        if (outFolderIndex)
            *outFolderIndex = static_cast<mz_uint32>(e.folderIndex);

        if (outFilesInFolder)
            *outFilesInFolder = static_cast<mz_uint32>(e.filesInFolder);

        return true;
    }

    DebugLogWrite("WARN", "SCAN", "SOLID_INFO_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePath, (unsigned int)index);
    return false;
}

bool ArchiveExtractEntryToMemory(const char* archivePath, mz_uint index, void* outBuffer, size_t outBufferSize)
{
    if (!archivePath || !archivePath[0] || !outBuffer || outBufferSize == 0)
    {
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | bytes=%zu | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index, outBufferSize);
        return false;
    }

    if (IsSevenZipArchivePath(archivePath) || IsRarArchivePath(archivePath))
    {
        wchar_t widePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
        {
            DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
            return false;
        }

        std::vector<unsigned char> data;
        std::string error;

        if (!ArchiveReaderExtractEntryToMemoryLockedIfRar(archivePath, widePath, (uint32_t)index, data, error))
        {
            DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=backend_extract_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        if (data.size() > outBufferSize)
        {
            DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | size=%zu | buffer=%zu | reason=buffer_too_small", archivePath, (unsigned int)index, data.size(), outBufferSize);
            return false;
        }

        memcpy(outBuffer, data.data(), data.size());
        return true;
    }

    if (!IsZipArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
        return false;
    }

    /*
        ZIP memory extraction path.

        Normal ZIP and split ZIP both go through the backend vector extraction helper.
        archive_zip_reader.cpp now owns ZIP opening and knows how to expose .zip.001,
        .zip.002, etc. as one logical miniz callback stream. Keeping a direct
        mz_zip_reader_init_file() path here would bypass that split ZIP support and
        fail by opening only the .001 part.

        The vector helper returns the full extracted file. This wrapper only copies it
        into the caller-provided buffer after verifying the size.
    */
    {
        std::vector<unsigned char> data;
        std::string error;

        if (!ArchiveExtractEntryToMemory(archivePath, (uint32_t)index, data, error))
        {
            DebugLogWrite("ERROR", "ZIP", "MEMORY_EXTRACT_FAIL", "archive=%s | index=%u | reason=zip_vector_extract_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        if (data.size() > outBufferSize)
        {
            DebugLogWrite("ERROR", "ZIP", "MEMORY_EXTRACT_FAIL", "archive=%s | index=%u | bytes=%zu | buffer=%zu | reason=output_buffer_too_small", archivePath, (unsigned int)index, data.size(), outBufferSize);
            return false;
        }

        if (!data.empty())
        {
            memcpy(outBuffer, data.data(), data.size());
        }

        return true;
    }
}

// -----------------------------------------------------------------------------
// ArchiveExtractEntryToMemory
//
// Extracts one archive entry into a std::vector using the backend-native
// extraction path.
//
// This function must not call the raw-buffer overload:
//
//     ArchiveExtractEntryToMemory(const char*, mz_uint, void*, size_t)
//
// The raw-buffer overload may itself call this vector overload for ZIP so split
// ZIP archives can use archive_zip_reader.cpp's miniz callback reader. Calling
// back into the raw-buffer overload from here creates ZIP recursion and can crash
// Winamp.
//
// Instead, this function converts the archive path to wide form and calls the
// backend extraction helper directly. That lets ZIP, split ZIP, 7z, split 7z,
// RAR, and multipart RAR all use their correct backend reader.
// -----------------------------------------------------------------------------
bool ArchiveExtractEntryToMemory(const char* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    data.clear();
    error.clear();

    if (!archivePath || !archivePath[0])
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=empty_archive_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    if (!IsZipArchivePath(archivePath) && !IsSevenZipArchivePath(archivePath) && !IsRarArchivePath(archivePath))
    {
        error = "Unsupported archive type";
        DebugLogWrite("WARN", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
        return false;
    }

    wchar_t widePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
    {
        error = "Failed to convert archive path";
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
        return false;
    }

    if (!ArchiveReaderExtractEntryToMemoryLockedIfRar(archivePath, widePath, index, data, error))
    {
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=backend_extract_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
        return false;
    }

    if (data.empty())
    {
        error = "Backend returned no extracted data";
        DebugLogWrite("ERROR", "EXTRACT", "MEMORY_FAIL", "archive=%s | index=%u | reason=empty_extracted_data", archivePath, (unsigned int)index);
        return false;
    }

    DebugLogWrite("INFO", "EXTRACT", "MEMORY_OK", "archive=%s | index=%u | bytes=%zu | route=backend_vector", archivePath, (unsigned int)index, data.size());

    return true;
}






bool ArchiveOpenEntryStream(ArchiveStreamContext* ctx, const char* archivePath, mz_uint index)
{
    if (!ctx || !archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "archive=%s | index=%u | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    // Do not discard an already-owned handle by zeroing the context. The
    // original Winamp path and generic backend have the same close-before-open
    // lifecycle requirement.
    if (ctx->libarchiveZipStream || ctx->libarchiveZipOpen || ctx->backendStream.impl || ctx->backendOpen)
    {
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | reason=legacy_stream_already_open", (unsigned int)index);
        return false;
    }

    if (strnlen_s(archivePath, sizeof(ctx->archivePath)) >= sizeof(ctx->archivePath))
    {
        DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "index=%u | reason=archive_path_too_long", (unsigned int)index);
        return false;
    }

    *ctx = ArchiveStreamContext{};
    strcpy_s(ctx->archivePath, sizeof(ctx->archivePath), archivePath);
    ctx->index = index;

    if (IsZipArchivePath(archivePath))
    {
        wchar_t widePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
        {
            DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "archive_type=ZIP | archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
            return false;
        }

        std::string error;
        uint64_t entrySize = 0;

        if (!LibarchiveZipOpenEntryStream(widePath, static_cast<uint32_t>(index), &ctx->libarchiveZipStream, &entrySize, error))
        {
            ctx->libarchiveZipOpen = false;
            ctx->libarchiveZipStream = nullptr;
            DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "archive_type=ZIP | archive=%s | index=%u | route=libarchive_zip | reason=open_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        ctx->libarchiveZipOpen = true;
        ctx->position = 0;

        DebugLogWrite("INFO", "STREAM", "OPEN_OK", "archive_type=ZIP | archive=%s | index=%u | route=libarchive_zip | entry_size=%llu", archivePath, (unsigned int)index, (unsigned long long)entrySize);

        return true;
    }

    if (IsSevenZipArchivePath(archivePath) || IsRarArchivePath(archivePath))
    {
        wchar_t widePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, widePath, sizeof(widePath) / sizeof(widePath[0])))
        {
            DebugLogWrite("ERROR", "STREAM", "OPEN_FAIL", "archive=%s | index=%u | reason=path_convert_failed", archivePath, (unsigned int)index);
            return false;
        }

        std::string error;
        bool rarLockHeld = false;

        if (!ArchiveReaderOpenEntryStreamLockedIfRar(archivePath, widePath, static_cast<uint32_t>(index), ctx->backendStream, error, &rarLockHeld))
        {
            DebugLogWrite("WARN", "STREAM", "OPEN_FAIL", "archive=%s | index=%u | reason=backend_open_failed | error=%s", archivePath, (unsigned int)index, error.c_str());
            return false;
        }

        ctx->backendOpen = true;
        ctx->position = 0;

        if (rarLockHeld)
        {
            DebugLogWrite("INFO", "STREAM", "RAR_STREAM_LOCK_HELD", "archive=%s | index=%u | reason=stream_lifetime", archivePath, (unsigned int)index);
        }

        return true;
    }

    DebugLogWrite("WARN", "STREAM", "OPEN_FAIL", "archive=%s | index=%u | reason=unsupported_archive", archivePath, (unsigned int)index);
    return false;
}

size_t ArchiveReadEntryStream(ArchiveStreamContext* ctx, void* bufferOut, size_t bytesToRead)
{
    if (!ctx || !bufferOut || bytesToRead == 0)
    {
        return 0;
    }

    if (ctx->libarchiveZipOpen && ctx->libarchiveZipStream)
    {
        std::string error;
        size_t bytesRead = LibarchiveZipReadEntryStream(ctx->libarchiveZipStream, bufferOut, bytesToRead, error);

        if (bytesRead == 0 && !error.empty())
        {
            DebugLogWrite("ERROR", "STREAM", "READ_FAIL", "archive=%s | index=%u | route=libarchive_zip | requested=%zu | reason=read_failed | error=%s", ctx->archivePath, (unsigned int)ctx->index, bytesToRead, error.c_str());
        }

        ctx->position += bytesRead;
        return bytesRead;
    }

    if (ctx->backendOpen)
    {
        size_t bytesRead = ::ArchiveReadEntryStream(ctx->backendStream, bufferOut, bytesToRead);

        /*
        Backend stream implementations should keep backendStream.position updated.
        Mirror it here for decoder tell/seek callbacks.
        */
        ctx->position = static_cast<mz_uint64>(ctx->backendStream.position);

        /*
        If a future backend implementation returns bytes but forgets to update position,
        keep forward progress sane.
        */
        if (bytesRead > 0 && ctx->position == 0)
        {
            ctx->position = static_cast<mz_uint64>(bytesRead);
        }

        return bytesRead;
    }

    return 0;
}

bool ArchiveOpenValidatedEntryStream(ArchiveStreamContext* ctx, const char* archivePath, mz_uint index, size_t probeBytes, std::string* outError)
{
    if (outError)
    {
        outError->clear();
    }

    if (!ctx)
    {
        if (outError)
        {
            *outError = "invalid_stream_context";
        }

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "archive=%s | index=%u | reason=invalid_stream_context", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    // The open routine rejects active contexts; never erase an existing
    // stream before it has been closed.
    if (ctx->libarchiveZipStream || ctx->libarchiveZipOpen || ctx->backendStream.impl || ctx->backendOpen)
    {
        if (outError)
            *outError = "stream_already_open";

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "index=%u | reason=stream_already_open", (unsigned int)index);
        return false;
    }

    *ctx = ArchiveStreamContext{};

    if (!archivePath || !archivePath[0])
    {
        if (outError)
        {
            *outError = "empty_archive_path";
        }

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "archive=%s | index=%u | reason=empty_archive_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    if (probeBytes == 0)
    {
        probeBytes = 4096;
    }

    if (!ArchiveOpenEntryStream(ctx, archivePath, index))
    {
        if (outError)
        {
            *outError = "open_entry_stream_failed";
        }

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "archive=%s | index=%u | probe_bytes=%zu | reason=open_entry_stream_failed", archivePath, (unsigned int)index, probeBytes);
        return false;
    }

    unsigned char probe[16384];
    size_t wanted = probeBytes;

    if (wanted > sizeof(probe))
    {
        wanted = sizeof(probe);
    }

    size_t got = ArchiveReadEntryStream(ctx, probe, wanted);

    if (got == 0)
    {
        ArchiveCloseEntryStream(ctx);
        *ctx = ArchiveStreamContext{};

        if (outError)
        {
            *outError = "first_read_returned_zero";
        }

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "archive=%s | index=%u | probe_bytes=%zu | reason=first_read_returned_zero", archivePath, (unsigned int)index, wanted);
        return false;
    }

    if (!ArchiveSeekEntryStream(ctx, 0))
    {
        ArchiveCloseEntryStream(ctx);
        *ctx = ArchiveStreamContext{};

        if (outError)
        {
            *outError = "rewind_after_probe_failed";
        }

        DebugLogWrite("WARN", "STREAM", "OPEN_VALIDATED_FAIL", "archive=%s | index=%u | probe_bytes=%zu | bytes_read=%zu | reason=rewind_after_probe_failed", archivePath, (unsigned int)index, wanted, got);
        return false;
    }

    DebugLogWrite("INFO", "STREAM", "OPEN_VALIDATED_OK", "archive=%s | index=%u | probe_bytes=%zu | bytes_read=%zu", archivePath, (unsigned int)index, wanted, got);
    return true;
}

bool ArchiveValidateEntryStreamRead(const char* archivePath, mz_uint index, size_t probeBytes, std::string* outError)
{
    if (outError)
    {
        outError->clear();
    }

    if (!archivePath || !archivePath[0])
    {
        if (outError)
        {
            *outError = "empty_archive_path";
        }

        DebugLogWrite("WARN", "STREAM", "PREFLIGHT_READ_FAIL", "archive=%s | index=%u | reason=empty_archive_path", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    if (probeBytes == 0)
    {
        probeBytes = 4096;
    }

    ArchiveStreamContext ctx{};

    if (!ArchiveOpenEntryStream(&ctx, archivePath, index))
    {
        if (outError)
        {
            *outError = "open_entry_stream_failed";
        }

        DebugLogWrite("WARN", "STREAM", "PREFLIGHT_READ_FAIL", "archive=%s | index=%u | probe_bytes=%zu | reason=open_entry_stream_failed", archivePath, (unsigned int)index, probeBytes);
        return false;
    }

    unsigned char probe[16384];
    size_t wanted = probeBytes;

    if (wanted > sizeof(probe))
    {
        wanted = sizeof(probe);
    }

    size_t got = ArchiveReadEntryStream(&ctx, probe, wanted);

    if (got == 0)
    {
        ArchiveCloseEntryStream(&ctx);

        if (outError)
        {
            *outError = "first_read_returned_zero";
        }

        DebugLogWrite("WARN", "STREAM", "PREFLIGHT_READ_FAIL", "archive=%s | index=%u | probe_bytes=%zu | reason=first_read_returned_zero", archivePath, (unsigned int)index, wanted);
        return false;
    }

    ArchiveCloseEntryStream(&ctx);

    DebugLogWrite("INFO", "STREAM", "PREFLIGHT_READ_OK", "archive=%s | index=%u | probe_bytes=%zu | bytes_read=%zu", archivePath, (unsigned int)index, wanted, got);
    return true;
}

bool ArchiveSeekEntryStream(ArchiveStreamContext* ctx, mz_uint64 absolutePosition)
{
    if (!ctx)
    {
        DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "target=%llu | reason=null_context", (unsigned long long)absolutePosition);
        return false;
    }

    if (ctx->backendOpen)
    {
        if (!::ArchiveSeekEntryStream(ctx->backendStream, (uint64_t)absolutePosition))
        {
            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "archive=%s | index=%u | target=%llu | route=backend", ctx->archivePath, (unsigned int)ctx->index, (unsigned long long)absolutePosition);
            return false;
        }

        ctx->position = static_cast<mz_uint64>(ctx->backendStream.position);
        return true;
    }

    if (ctx->libarchiveZipOpen && ctx->libarchiveZipStream)
    {
        std::string error;

        if (!LibarchiveZipSeekEntryStream(ctx->libarchiveZipStream, static_cast<uint64_t>(absolutePosition), error))
        {
            DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "archive=%s | index=%u | target=%llu | route=libarchive_zip | reason=seek_failed | error=%s", ctx->archivePath, (unsigned int)ctx->index, (unsigned long long)absolutePosition, error.c_str());
            return false;
        }

        ctx->position = absolutePosition;
        return true;
    }

    

    DebugLogWrite("WARN", "STREAM", "SEEK_FAIL", "target=%llu | reason=no_open_stream", (unsigned long long)absolutePosition);
    return false;
}

void ArchiveCloseEntryStream(ArchiveStreamContext* ctx)
{
    if (!ctx)
        return;

    if (ctx->libarchiveZipOpen || ctx->libarchiveZipStream)
    {
        LibarchiveZipCloseEntryStream(ctx->libarchiveZipStream);
        ctx->libarchiveZipStream = nullptr;
        ctx->libarchiveZipOpen = false;
    }
    
    if (ctx->backendOpen)
    {
        ::ArchiveCloseEntryStream(ctx->backendStream);
        ctx->backendOpen = false;
    }

    ctx->position = 0;
}