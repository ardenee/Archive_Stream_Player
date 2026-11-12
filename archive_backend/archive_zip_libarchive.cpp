#define NOMINMAX

#ifndef LIBARCHIVE_STATIC
#define LIBARCHIVE_STATIC
#endif

#include "archive_zip_libarchive.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "debug_log.h"

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



struct LibarchiveZipPart
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    uint64_t size = 0;
    std::string path;
};

struct LibarchiveZipSource
{
    uint64_t totalSize = 0;
    uint64_t position = 0;
    std::vector<LibarchiveZipPart> parts;
    unsigned char buffer[256 * 1024]{};
};

static bool LaZipUtf8EndsWithInsensitive(const std::string& value, const char* suffix)
{
    if (!suffix)
    {
        return false;
    }

    size_t valueLen = value.length();
    size_t suffixLen = strlen(suffix);

    if (valueLen < suffixLen)
    {
        return false;
    }

    return _stricmp(value.c_str() + valueLen - suffixLen, suffix) == 0;
}

static bool LaZipIsSplitZipFirstVolume(const std::string& archivePathUtf8)
{
    return LaZipUtf8EndsWithInsensitive(archivePathUtf8, ".zip.001");
}

static bool LaZipBuildSplitPartPath(const std::string& firstVolumePath, unsigned int volumeNumber, std::string& outPath)
{
    outPath.clear();

    if (firstVolumePath.length() < 3)
    {
        DebugLogWrite("ERROR", "LAZIP", "SPLIT_PATH_FAIL", "first_volume=%s | volume=%u | reason=path_too_short", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    if (volumeNumber == 0 || volumeNumber > 999)
    {
        DebugLogWrite("ERROR", "LAZIP", "SPLIT_PATH_FAIL", "first_volume=%s | volume=%u | reason=invalid_volume_number", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    char suffix[8];
    sprintf_s(suffix, sizeof(suffix), "%03u", volumeNumber);

    outPath = firstVolumePath.substr(0, firstVolumePath.length() - 3);
    outPath += suffix;

    return true;
}

static void LaZipCloseSource(LibarchiveZipSource* source)
{
    if (!source)
    {
        return;
    }

    for (LibarchiveZipPart& part : source->parts)
    {
        if (part.handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(part.handle);
            part.handle = INVALID_HANDLE_VALUE;
        }
    }

    source->parts.clear();
    source->totalSize = 0;
    source->position = 0;
}

// ZIP source paths are UTF-8 internally. Windows file APIs must use UTF-16;
// CreateFileA/GetFileAttributesA interpret the path in the active ANSI codepage.
static bool LaZipUtf8PathToWide(const std::string& utf8Path, std::wstring& widePath)
{
    widePath.clear();

    if (utf8Path.empty())
        return false;

    int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.c_str(), -1, nullptr, 0);
    if (needed <= 1)
        return false;

    widePath.resize(static_cast<size_t>(needed));
    int written = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8Path.c_str(), -1, &widePath[0], needed);
    if (written != needed)
    {
        widePath.clear();
        return false;
    }

    widePath.resize(static_cast<size_t>(needed - 1));
    return true;
}

static bool LaZipAddPart(LibarchiveZipSource* source, const std::string& path)
{
    if (!source || path.empty())
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=invalid_input", path.c_str());
        return false;
    }

    std::wstring widePath;
    if (!LaZipUtf8PathToWide(path, widePath))
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=utf8_path_conversion_failed", path.c_str());
        return false;
    }

    DWORD attrs = GetFileAttributesW(widePath.c_str());

    if (attrs == INVALID_FILE_ATTRIBUTES)
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=missing | winerr=%lu", path.c_str(), GetLastError());
        return false;
    }

    if (attrs & FILE_ATTRIBUTE_DIRECTORY)
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=is_directory", path.c_str());
        return false;
    }

    HANDLE handle = CreateFileW(widePath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

    if (handle == INVALID_HANDLE_VALUE)
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=open_failed | winerr=%lu", path.c_str(), GetLastError());
        return false;
    }

    LARGE_INTEGER fileSize;

    if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0)
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=get_size_failed | winerr=%lu", path.c_str(), GetLastError());
        CloseHandle(handle);
        return false;
    }

    LibarchiveZipPart part;
    part.handle = handle;
    part.size = static_cast<uint64_t>(fileSize.QuadPart);
    part.path = path;

    try
    {
        source->parts.push_back(part);
    }
    catch (const std::bad_alloc&)
    {
        DebugLogWrite("ERROR", "LAZIP", "OPEN_PART_FAIL", "path=%s | reason=part_list_alloc_failed", path.c_str());
        CloseHandle(handle);
        return false;
    }

    source->totalSize += part.size;
    return true;
}

static bool LaZipOpenSource(const std::string& archivePathUtf8, LibarchiveZipSource* source, std::string& error)
{
    if (!source)
    {
        error = "Invalid libarchive ZIP source";
        DebugLogWrite("ERROR", "LAZIP", "OPEN_SOURCE_FAIL", "archive=%s | reason=invalid_source", archivePathUtf8.c_str());
        return false;
    }

    LaZipCloseSource(source);

    if (LaZipIsSplitZipFirstVolume(archivePathUtf8))
    {
        bool openedAny = false;

        for (unsigned int volumeNumber = 1; volumeNumber <= 999; ++volumeNumber)
        {
            std::string partPath;

            if (!LaZipBuildSplitPartPath(archivePathUtf8, volumeNumber, partPath))
            {
                error = "Failed to build split ZIP part path";
                LaZipCloseSource(source);
                return false;
            }

            std::wstring widePartPath;
            if (!LaZipUtf8PathToWide(partPath, widePartPath))
            {
                error = "Failed to convert split ZIP part path to UTF-16";
                LaZipCloseSource(source);
                return false;
            }

            DWORD attrs = GetFileAttributesW(widePartPath.c_str());

            if (attrs == INVALID_FILE_ATTRIBUTES)
            {
                if (!openedAny)
                {
                    error = "Split ZIP first volume is missing";
                    DebugLogWrite("ERROR", "LAZIP", "OPEN_SOURCE_FAIL", "archive=%s | part=%s | reason=first_volume_missing | winerr=%lu", archivePathUtf8.c_str(), partPath.c_str(), GetLastError());
                    LaZipCloseSource(source);
                    return false;
                }

                break;
            }

            if (!LaZipAddPart(source, partPath))
            {
                error = "Failed to open split ZIP part";
                LaZipCloseSource(source);
                return false;
            }

            openedAny = true;
        }

        if (!openedAny || source->parts.empty() || source->totalSize == 0)
        {
            error = "No usable split ZIP source data";
            DebugLogWrite("ERROR", "LAZIP", "OPEN_SOURCE_FAIL", "archive=%s | reason=no_split_data", archivePathUtf8.c_str());
            LaZipCloseSource(source);
            return false;
        }

        //DebugLogWrite("INFO", "LAZIP", "OPEN_SOURCE_OK", "archive=%s | split=1 | parts=%zu | total_size=%llu", archivePathUtf8.c_str(), source->parts.size(), (unsigned long long)source->totalSize);
        return true;
    }

    if (!LaZipAddPart(source, archivePathUtf8))
    {
        error = "Failed to open ZIP file";
        LaZipCloseSource(source);
        return false;
    }

    //DebugLogWrite("INFO", "LAZIP", "OPEN_SOURCE_OK", "archive=%s | split=0 | parts=1 | total_size=%llu", archivePathUtf8.c_str(), (unsigned long long)source->totalSize);
    return true;
}

static la_ssize_t LaZipReadCallback(struct archive* archiveHandle, void* clientData, const void** buffer)
{
    (void)archiveHandle;

    LibarchiveZipSource* source = static_cast<LibarchiveZipSource*>(clientData);

    if (!source || !buffer)
    {
        return ARCHIVE_FATAL;
    }

    if (source->position >= source->totalSize)
    {
        *buffer = source->buffer;
        return 0;
    }

    size_t maxRead = sizeof(source->buffer);
    size_t totalRead = 0;

    while (totalRead < maxRead && source->position < source->totalSize)
    {
        uint64_t partBase = 0;
        size_t partIndex = 0;
        bool foundPart = false;

        for (size_t i = 0; i < source->parts.size(); ++i)
        {
            uint64_t partEnd = partBase + source->parts[i].size;

            if (source->position >= partBase && source->position < partEnd)
            {
                partIndex = i;
                foundPart = true;
                break;
            }

            partBase = partEnd;
        }

        if (!foundPart)
        {
            DebugLogWrite("ERROR", "LAZIP", "READ_FAIL", "position=%llu | total_size=%llu | reason=part_not_found", (unsigned long long)source->position, (unsigned long long)source->totalSize);
            return ARCHIVE_FATAL;
        }

        LibarchiveZipPart& part = source->parts[partIndex];

        uint64_t partOffset64 = source->position - partBase;
        uint64_t partRemaining64 = part.size - partOffset64;
        size_t wanted = maxRead - totalRead;

        if (partRemaining64 < static_cast<uint64_t>(wanted))
        {
            wanted = static_cast<size_t>(partRemaining64);
        }

        LARGE_INTEGER seekPos;
        seekPos.QuadPart = static_cast<LONGLONG>(partOffset64);

        if (!SetFilePointerEx(part.handle, seekPos, NULL, FILE_BEGIN))
        {
            DebugLogWrite("ERROR", "LAZIP", "READ_FAIL", "part=%zu | path=%s | offset=%llu | reason=seek_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, GetLastError());
            return ARCHIVE_FATAL;
        }

        DWORD bytesRead = 0;

        if (!ReadFile(part.handle, source->buffer + totalRead, static_cast<DWORD>(wanted), &bytesRead, NULL))
        {
            DebugLogWrite("ERROR", "LAZIP", "READ_FAIL", "part=%zu | path=%s | offset=%llu | requested=%zu | reason=read_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, wanted, GetLastError());
            return ARCHIVE_FATAL;
        }

        if (bytesRead == 0)
        {
            break;
        }

        totalRead += bytesRead;
        source->position += bytesRead;
    }

    *buffer = source->buffer;
    return static_cast<la_ssize_t>(totalRead);
}

static la_int64_t LaZipSeekCallback(struct archive* archiveHandle, void* clientData, la_int64_t offset, int whence)
{
    (void)archiveHandle;

    LibarchiveZipSource* source = static_cast<LibarchiveZipSource*>(clientData);

    if (!source)
    {
        return -1;
    }

    int64_t newPos = 0;

    if (whence == SEEK_SET)
    {
        newPos = static_cast<int64_t>(offset);
    }
    else if (whence == SEEK_CUR)
    {
        newPos = static_cast<int64_t>(source->position) + static_cast<int64_t>(offset);
    }
    else if (whence == SEEK_END)
    {
        newPos = static_cast<int64_t>(source->totalSize) + static_cast<int64_t>(offset);
    }
    else
    {
        DebugLogWrite("ERROR", "LAZIP", "SEEK_FAIL", "offset=%lld | whence=%d | reason=unsupported_whence", (long long)offset, whence);
        return -1;
    }

    if (newPos < 0)
    {
        DebugLogWrite("ERROR", "LAZIP", "SEEK_FAIL", "offset=%lld | whence=%d | reason=negative_position", (long long)offset, whence);
        return -1;
    }

    source->position = static_cast<uint64_t>(newPos);
    return static_cast<la_int64_t>(source->position);
}

static la_int64_t LaZipSkipCallback(struct archive* archiveHandle, void* clientData, la_int64_t request)
{
    (void)archiveHandle;

    LibarchiveZipSource* source = static_cast<LibarchiveZipSource*>(clientData);

    if (!source || request < 0)
    {
        return 0;
    }

    uint64_t available = 0;

    if (source->position < source->totalSize)
    {
        available = source->totalSize - source->position;
    }

    uint64_t skip = static_cast<uint64_t>(request);

    if (skip > available)
    {
        skip = available;
    }

    source->position += skip;
    return static_cast<la_int64_t>(skip);
}

static int LaZipCloseCallback(struct archive* archiveHandle, void* clientData)
{
    (void)archiveHandle;

    LibarchiveZipSource* source = static_cast<LibarchiveZipSource*>(clientData);
    LaZipCloseSource(source);

    return ARCHIVE_OK;
}

// -----------------------------------------------------------------------------
// LaZipAddSavedPasswords
//
// Adds saved keep.txt password candidates to a newly created libarchive reader.
//
// This never logs password values. It only logs the keep.txt path and candidate
// count when there are candidates, which is needed while bringing up encrypted
// ZIP support.
// -----------------------------------------------------------------------------
static int LaZipAddSavedPasswords(struct archive* archiveHandle, const std::string& archivePathUtf8)
{
    if (!archiveHandle)
    {
        return 0;
    }

    ArchivePasswordList passwordList;
    std::string passwordError;

    if (!ArchivePasswordLoadList(passwordList, passwordError))
    {
        DebugLogWrite("ERROR", "PASSWORD", "LOAD_FAIL", "archive=%s | backend=libarchive_zip | path=%s | error=%s", archivePathUtf8.c_str(), ArchivePasswordGetListPath(), passwordError.c_str());
        return 0;
    }

    if (!passwordList.passwords.empty())
    {
        DebugLogWrite("WARN", "PASSWORD", "CANDIDATES_LOADED", "archive=%s | backend=libarchive_zip | path=%s | count=%zu", archivePathUtf8.c_str(), ArchivePasswordGetListPath(), passwordList.passwords.size());
    }

    int added = 0;

    for (const std::string& password : passwordList.passwords)
    {
        if (password.empty())
        {
            continue;
        }

        int result = archive_read_add_passphrase(archiveHandle, password.c_str());

        if (result != ARCHIVE_OK)
        {
            const char* err = archive_error_string(archiveHandle);
            DebugLogWrite("ERROR", "PASSWORD", "ADD_FAIL", "archive=%s | backend=libarchive_zip | error=%s", archivePathUtf8.c_str(), err ? err : "archive_read_add_passphrase_failed");
            continue;
        }

        ++added;
    }

    std::vector<std::string> runtimePasswords;
    ArchivePasswordGetRuntimePasswords(archivePathUtf8.c_str(), runtimePasswords);

    for (const std::string& password : runtimePasswords)
    {
        if (password.empty())
        {
            continue;
        }

        int result = archive_read_add_passphrase(archiveHandle, password.c_str());

        if (result == ARCHIVE_OK)
        {
            ++added;
        }
    }

    if (!runtimePasswords.empty())
    {
        DebugLogWrite("WARN", "PASSWORD", "RUNTIME_CANDIDATES_LOADED", "archive=%s | backend=libarchive_zip | count=%zu", archivePathUtf8.c_str(), runtimePasswords.size());
    }

    if (!passwordList.passwords.empty() && added == 0)
    {
        DebugLogWrite("ERROR", "PASSWORD", "ADD_ALL_FAIL", "archive=%s | backend=libarchive_zip | candidates=%zu", archivePathUtf8.c_str(), passwordList.passwords.size());
    }

    return added;
}
static bool LaZipOpenArchive(struct archive** outArchive, LibarchiveZipSource* source, const std::string& archivePathUtf8, const std::vector<std::string>* runtimePasswords, std::string& error)
{
    if (outArchive)
    {
        *outArchive = nullptr;
    }

    if (!outArchive || !source)
    {
        error = "Invalid libarchive open inputs";
        return false;
    }

    if (!LaZipOpenSource(archivePathUtf8, source, error))
    {
        return false;
    }

    struct archive* a = archive_read_new();

    if (!a)
    {
        error = "archive_read_new failed";
        DebugLogWrite("ERROR", "LAZIP", "OPEN_FAIL", "archive=%s | reason=archive_read_new_failed", archivePathUtf8.c_str());
        LaZipCloseSource(source);
        return false;
    }

    archive_read_support_filter_none(a);
    archive_read_support_format_zip(a);

    /*
        Add saved keep.txt passwords before archive_read_open2().

        libarchive requires passphrases to be registered before encrypted entries
        are read. This is silent candidate injection only; UI prompting will be added
        in the input plugin after we have a clean backend status path.
    */
    int passwordCandidateCount = LaZipAddSavedPasswords(a, archivePathUtf8);

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

    /*
        Register the seek callback before archive_read_open2().
        Register the seek callback before archive_read_open2().

        libarchive requires callback configuration while the archive object is still
        in the "new" state. Calling archive_read_set_seek_callback() after opening
        moves the object into the header/read state and causes an internal error on
        the first archive_read_next_header() call.
    */
    if (archive_read_set_seek_callback(a, LaZipSeekCallback) != ARCHIVE_OK)
    {
        const char* err = archive_error_string(a);
        error = err ? err : "archive_read_set_seek_callback failed";
        DebugLogWrite("ERROR", "LAZIP", "OPEN_FAIL", "archive=%s | reason=set_seek_callback_failed | error=%s", archivePathUtf8.c_str(), error.c_str());
        archive_read_free(a);
        LaZipCloseSource(source);
        return false;
    }

    int openResult = archive_read_open2(a, source, NULL, LaZipReadCallback, LaZipSkipCallback, LaZipCloseCallback);

    if (openResult != ARCHIVE_OK)
    {
        const char* err = archive_error_string(a);
        error = err ? err : "archive_read_open2 failed";
        DebugLogWrite("ERROR", "LAZIP", "OPEN_FAIL", "archive=%s | reason=archive_read_open2_failed | error=%s", archivePathUtf8.c_str(), error.c_str());
        archive_read_free(a);
        LaZipCloseSource(source);
        return false;
    }

    *outArchive = a;
    return true;
}

static bool LaZipWcharToUtf8(const wchar_t* input, std::string& output)
{
    output.clear();

    if (!input || !*input)
    {
        return false;
    }

    int needed = WideCharToMultiByte(CP_UTF8, 0, input, -1, NULL, 0, NULL, NULL);

    if (needed <= 1)
    {
        return false;
    }

    // Allocate space for the terminating NUL before the Windows conversion.
    output.resize(static_cast<size_t>(needed));
    int written = WideCharToMultiByte(CP_UTF8, 0, input, -1, &output[0], needed, NULL, NULL);

    if (written != needed)
    {
        output.clear();
        return false;
    }

    output.resize(static_cast<size_t>(needed - 1));
    return true;
}

static bool LaZipIsSupportedAudioPath(const char* path)
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

    return
        _stricmp(ext, ".mp3") == 0 ||
        _stricmp(ext, ".wav") == 0 ||
        _stricmp(ext, ".flac") == 0 ||
        _stricmp(ext, ".ogg") == 0 ||
        _stricmp(ext, ".wma") == 0 ||
        _stricmp(ext, ".m4a") == 0 ||
        _stricmp(ext, ".aac") == 0;
}

bool LibarchiveZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    entries.clear();
    error.clear();

    std::string archivePathUtf8;

    if (!LaZipWcharToUtf8(archivePath, archivePathUtf8))
    {
        error = "Failed to convert ZIP path to UTF-8";
        DebugLogWrite("ERROR", "LAZIP", "LIST_FAIL", "reason=path_convert_failed");
        return false;
    }

    LibarchiveZipSource source;
    struct archive* a = nullptr;

    if (!LaZipOpenArchive(&a, &source, archivePathUtf8, nullptr, error))
    {
        DebugLogWrite("ERROR", "LAZIP", "LIST_FAIL", "archive=%s | reason=open_failed | error=%s", archivePathUtf8.c_str(), error.c_str());
        return false;
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
            DebugLogWrite("ERROR", "LAZIP", "LIST_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)index, error.c_str());
            archive_read_free(a);
            LaZipCloseSource(&source);
            return false;
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
        bool supportedAudio = !isFolder && LaZipIsSupportedAudioPath(path);

        ArchiveEntry out;
        out.index = index;
        out.path = path;
        out.size = archive_entry_size_is_set(entry) ? static_cast<uint64_t>(archive_entry_size(entry)) : 0;
        out.isFolder = isFolder;
        out.isSupportedAudio = supportedAudio;
        out.hasCrc32 = false;
        out.crc32 = 0;

        try
        {
            entries.push_back(out);
        }
        catch (const std::bad_alloc&)
        {
            error = "Out of memory while listing ZIP entries";
            DebugLogWrite("ERROR", "LAZIP", "LIST_FAIL", "archive=%s | index=%u | reason=entry_list_alloc_failed", archivePathUtf8.c_str(), (unsigned int)index);
            archive_read_free(a);
            LaZipCloseSource(&source);
            return false;
        }

        archive_read_data_skip(a);
        ++index;
    }

    archive_read_free(a);
    LaZipCloseSource(&source);

    //DebugLogWrite("INFO", "LAZIP", "LIST_OK", "archive=%s | entries=%zu", archivePathUtf8.c_str(), entries.size());

    return true;
}
// -----------------------------------------------------------------------------
// LaZipFindEntryByIndex
//
// Opens the ZIP through libarchive and advances to the requested entry index.
//
// libarchive is entry-iterator based, not random-access by index. This helper
// adapts the current plugin/backend API by walking headers until the requested
// entry is reached. The returned archive handle is positioned on the selected
// entry's data stream.
// -----------------------------------------------------------------------------
static bool LaZipFindEntryByIndex(struct archive** outArchive, LibarchiveZipSource* source, const wchar_t* archivePath, uint32_t targetIndex, std::string& archivePathUtf8, std::string& error)
{
    if (outArchive)
    {
        *outArchive = nullptr;
    }

    archivePathUtf8.clear();

    if (!outArchive || !source)
    {
        error = "Invalid libarchive find-entry inputs";
        DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "index=%u | reason=invalid_input", (unsigned int)targetIndex);
        return false;
    }

    if (!LaZipWcharToUtf8(archivePath, archivePathUtf8))
    {
        error = "Failed to convert ZIP path to UTF-8";
        DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "index=%u | reason=path_convert_failed", (unsigned int)targetIndex);
        return false;
    }

    struct archive* a = nullptr;

    if (!LaZipOpenArchive(&a, source, archivePathUtf8, nullptr, error))
    {
        DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "archive=%s | index=%u | reason=open_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)targetIndex, error.c_str());
        return false;
    }

    struct archive_entry* entry = nullptr;
    uint32_t currentIndex = 0;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            error = "ZIP entry index not found";
            DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePathUtf8.c_str(), (unsigned int)targetIndex);
            archive_read_free(a);
            LaZipCloseSource(source);
            return false;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)targetIndex, error.c_str());
            archive_read_free(a);
            LaZipCloseSource(source);
            return false;
        }

        if (currentIndex == targetIndex)
        {
            if (archive_entry_filetype(entry) == AE_IFDIR)
            {
                error = "ZIP entry is a folder";
                DebugLogWrite("ERROR", "LAZIP", "FIND_FAIL", "archive=%s | index=%u | reason=entry_is_folder", archivePathUtf8.c_str(), (unsigned int)targetIndex);
                archive_read_free(a);
                LaZipCloseSource(source);
                return false;
            }

            int encrypted = archive_entry_is_encrypted(entry);

            if (encrypted > 0)
            {
                DebugLogWrite("WARN", "PASSWORD", "ENCRYPTED_ENTRY", "backend=libarchive_zip | archive=%s | index=%u | route=extract_memory", archivePathUtf8.c_str(), (unsigned int)targetIndex);
            }

            *outArchive = a;
            return true;
        }

        archive_read_data_skip(a);
        ++currentIndex;
    }
}

// -----------------------------------------------------------------------------
// LibarchiveZipExtractEntryToMemory
//
// Extracts one ZIP entry into memory using libarchive.
//
// This replaces the old ZIP memory extraction route while keeping the public
// backend API stable. It works for both normal ZIP and split ZIP because both
// are opened through LaZipOpenArchive() and its logical multipart source.
// -----------------------------------------------------------------------------
bool LibarchiveZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    data.clear();
    error.clear();

    LibarchiveZipSource source;
    std::string archivePathUtf8;
    struct archive* a = nullptr;

    if (!LaZipFindEntryByIndex(&a, &source, archivePath, index, archivePathUtf8, error))
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
            DebugLogWrite("ERROR", "LAZIP", "EXTRACT_MEMORY_FAIL", "archive=%s | index=%u | reason=read_data_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)index, error.c_str());
            archive_read_free(a);
            LaZipCloseSource(&source);
            data.clear();
            return false;
        }

        try
        {
            size_t oldSize = data.size();
            data.resize(oldSize + static_cast<size_t>(bytesRead));
            memcpy(data.data() + oldSize, buffer, static_cast<size_t>(bytesRead));
        }
        catch (const std::bad_alloc&)
        {
            error = "Out of memory extracting ZIP entry";
            DebugLogWrite("ERROR", "LAZIP", "EXTRACT_MEMORY_FAIL", "archive=%s | index=%u | bytes=%zu | reason=alloc_failed", archivePathUtf8.c_str(), (unsigned int)index, data.size());
            archive_read_free(a);
            LaZipCloseSource(&source);
            data.clear();
            return false;
        }
    }

    archive_read_free(a);
    LaZipCloseSource(&source);

    if (data.empty())
    {
        error = "ZIP entry extracted to empty buffer";
        DebugLogWrite("ERROR", "LAZIP", "EXTRACT_MEMORY_FAIL", "archive=%s | index=%u | reason=empty_output", archivePathUtf8.c_str(), (unsigned int)index);
        return false;
    }

    return true;
}

struct LibarchiveZipEntryStream
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
    LibarchiveZipSource source;
    struct archive* archiveHandle = nullptr;
};

// -----------------------------------------------------------------------------
// LaZipCloseEntryStreamInternal
//
// Closes a partially or fully opened libarchive ZIP entry stream.
//
// archive_read_free() triggers the libarchive close callback, which closes the
// archive source handles. LaZipCloseSource() is still called afterwards because
// it is idempotent and protects against partially opened failure paths.
// -----------------------------------------------------------------------------
static void LaZipCloseEntryStreamInternal(LibarchiveZipEntryStream* stream)
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

    LaZipCloseSource(&stream->source);
    stream->position = 0;
}

// -----------------------------------------------------------------------------
// LaZipOpenEntryStreamInternal
//
// Opens the ZIP archive through libarchive and advances to the requested entry.
//
// libarchive is entry-iterator based, so this adapts the existing index-based
// backend API by walking headers until the target index is reached. The archive
// handle is left positioned at the selected entry's decoded data stream.
// -----------------------------------------------------------------------------
static bool LaZipOpenEntryStreamInternal(LibarchiveZipEntryStream* stream, std::string& error)
{
    if (!stream)
    {
        error = "Invalid libarchive ZIP stream";
        DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "reason=invalid_stream");
        return false;
    }

    LaZipCloseEntryStreamInternal(stream);

    std::string archivePathUtf8;

    if (!LaZipWcharToUtf8(stream->archivePath.c_str(), archivePathUtf8))
    {
        error = "Failed to convert ZIP path to UTF-8";
        DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "index=%u | reason=path_convert_failed", (unsigned int)stream->index);
        return false;
    }

    stream->archivePathUtf8 = archivePathUtf8;

    struct archive* a = nullptr;

    if (!LaZipOpenArchive(&a, &stream->source, archivePathUtf8, &stream->runtimePasswords, error))
    {
        DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=open_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)stream->index, error.c_str());
        return false;
    }

    struct archive_entry* entry = nullptr;
    uint32_t currentIndex = 0;

    for (;;)
    {
        int r = archive_read_next_header(a, &entry);

        if (r == ARCHIVE_EOF)
        {
            error = "ZIP stream entry index not found";
            DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=index_not_found", archivePathUtf8.c_str(), (unsigned int)stream->index);
            archive_read_free(a);
            stream->archiveHandle = nullptr;
            LaZipCloseSource(&stream->source);
            return false;
        }

        if (r != ARCHIVE_OK)
        {
            const char* err = archive_error_string(a);
            error = err ? err : "archive_read_next_header failed";
            DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=next_header_failed | error=%s", archivePathUtf8.c_str(), (unsigned int)stream->index, error.c_str());
            archive_read_free(a);
            stream->archiveHandle = nullptr;
            LaZipCloseSource(&stream->source);
            return false;
        }

        if (currentIndex == stream->index)
        {
            if (archive_entry_filetype(entry) == AE_IFDIR)
            {
                error = "ZIP stream entry is a folder";
                DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "archive=%s | index=%u | reason=entry_is_folder", archivePathUtf8.c_str(), (unsigned int)stream->index);
                archive_read_free(a);
                stream->archiveHandle = nullptr;
                LaZipCloseSource(&stream->source);
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
                DebugLogWrite("WARN", "PASSWORD", "ENCRYPTED_ENTRY", "backend=libarchive_zip | archive=%s | index=%u | entry=%s", archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
            }

            stream->archiveHandle = a;
            stream->position = 0;
            stream->entrySize = archive_entry_size_is_set(entry) ? static_cast<uint64_t>(archive_entry_size(entry)) : 0;

            return true;
        }

        archive_read_data_skip(a);
        ++currentIndex;
    }
}

// -----------------------------------------------------------------------------
// LibarchiveZipOpenEntryStream
//
// Opens one ZIP entry for streaming through libarchive.
//
// This is the ZIP streaming path. Normal ZIP and split ZIP both use the same
// callback source underneath LaZipOpenArchive().
// -----------------------------------------------------------------------------
bool LibarchiveZipOpenEntryStream(const wchar_t* archivePath, uint32_t index, void** outStream, uint64_t* outEntrySize, std::string& error, const ArchiveStreamCancellationToken* cancellation)
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
        error = "Invalid ZIP stream open input";
        DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "index=%u | reason=invalid_input", (unsigned int)index);
        return false;
    }

    LibarchiveZipEntryStream* stream = new (std::nothrow) LibarchiveZipEntryStream();

    if (!stream)
    {
        error = "Failed to allocate ZIP stream";
        DebugLogWrite("ERROR", "LAZIP", "STREAM_OPEN_FAIL", "index=%u | reason=stream_alloc_failed", (unsigned int)index);
        return false;
    }

    stream->archivePath = archivePath;
    stream->index = index;
    stream->cancellation = cancellation;

    if (!LaZipOpenEntryStreamInternal(stream, error))
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

// -----------------------------------------------------------------------------
// LaZipPromptAndReopenEncryptedStream
//
// Prompts the UI layer for a password, adds it as a runtime candidate, reopens
// the same libarchive ZIP entry, and optionally saves the password to keep.txt.
//
// This is intentionally stream-local and libarchive-first. RAR/7z legacy code is
// not involved.
// -----------------------------------------------------------------------------
static bool LaZipPromptAndReopenEncryptedStream(LibarchiveZipEntryStream* stream, std::string& error)
{
    if (!stream)
    {
        error = "Invalid ZIP password prompt stream";
        return false;
    }

    if (ArchivePasswordIsArchiveCancelled(stream->archivePathUtf8.c_str()))
    {
        error = "Password prompt already cancelled for archive";
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_SUPPRESSED", "backend=libarchive_zip | archive=%s | index=%u | reason=archive_cancelled", stream->archivePathUtf8.c_str(), (unsigned int)stream->index);
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
        DebugLogWrite("WARN", "PASSWORD", "PROMPT_EMPTY", "backend=libarchive_zip | archive=%s | index=%u | entry=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
        return false;
    }

    stream->runtimePasswords.push_back(password);

    if (!LaZipOpenEntryStreamInternal(stream, error))
    {
        DebugLogWrite("ERROR", "PASSWORD", "REOPEN_FAIL", "backend=libarchive_zip | archive=%s | index=%u | entry=%s | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str(), error.c_str());
        return false;
    }

    /*
        Always keep the entered password pending until a read proves it works.

        pendingPasswordSave only controls whether it is written to keeps.txt.
        pendingPasswordToSave is also used to add the working password to the
        in-process runtime cache for the rest of this archive.
    */
    stream->pendingPasswordSave = savePassword;
    stream->pendingPasswordToSave = password;

    SecureZeroMemory(password, sizeof(password));
    return true;
}

// -----------------------------------------------------------------------------
// LibarchiveZipReadEntryStream
//
// Reads decoded bytes from a libarchive ZIP entry stream.
// -----------------------------------------------------------------------------
size_t LibarchiveZipReadEntryStream(void* streamHandle, void* buffer, size_t bytesToRead, std::string& error)
{
    error.clear();

    LibarchiveZipEntryStream* stream = static_cast<LibarchiveZipEntryStream*>(streamHandle);

    if (!stream || !stream->archiveHandle || !buffer || bytesToRead == 0)
    {
        return 0;
    }

    if (stream->cancellation && stream->cancellation->IsCancellationRequested())
    {
        error = "ZIP stream cancelled";
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
            DebugLogWrite("WARN", "PASSWORD", "READ_ABORTED_ARCHIVE_CANCELLED", "backend=libarchive_zip | archive=%s | index=%u | entry=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str());
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
            DebugLogWrite("WARN", "PASSWORD", "READ_REQUIRES_PASSWORD", "backend=libarchive_zip | archive=%s | index=%u | entry=%s | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, stream->entryNameUtf8.c_str(), firstErr ? firstErr : "archive_read_data_failed");
        }

        if (LaZipPromptAndReopenEncryptedStream(stream, error))
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
            DebugLogWrite("ERROR", "LAZIP", "STREAM_READ_FAIL", "index=%u | position=%llu | requested=%zu | reason=read_data_failed | error=%s", (unsigned int)stream->index, (unsigned long long)stream->position, bytesToRead, error.c_str());
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
                DebugLogWrite("ERROR", "PASSWORD", "SAVE_FAIL", "backend=libarchive_zip | archive=%s | index=%u | error=%s", stream->archivePathUtf8.c_str(), (unsigned int)stream->index, saveError.c_str());
            }
        }

        stream->pendingPasswordSave = false;
        SecureZeroMemory(&stream->pendingPasswordToSave[0], stream->pendingPasswordToSave.size());
        stream->pendingPasswordToSave.clear();
    }

    stream->position += static_cast<uint64_t>(bytesRead);
    return static_cast<size_t>(bytesRead);
}

// -----------------------------------------------------------------------------
// LibarchiveZipSeekEntryStream
//
// Seeks within a ZIP entry stream.
//
// Forward seek discards decoded bytes. Backward seek reopens the archive and
// discards forward to the requested absolute byte offset.
// -----------------------------------------------------------------------------
bool LibarchiveZipSeekEntryStream(void* streamHandle, uint64_t absolutePosition, std::string& error)
{
    error.clear();

    LibarchiveZipEntryStream* stream = static_cast<LibarchiveZipEntryStream*>(streamHandle);

    if (!stream)
    {
        error = "Invalid ZIP stream seek input";
        DebugLogWrite("ERROR", "LAZIP", "STREAM_SEEK_FAIL", "target=%llu | reason=invalid_stream", (unsigned long long)absolutePosition);
        return false;
    }

    if (stream->cancellation && stream->cancellation->IsCancellationRequested())
    {
        error = "ZIP seek cancelled";
        return false;
    }

    if (absolutePosition == stream->position)
    {
        return true;
    }

    if (absolutePosition < stream->position)
    {
        if (!LaZipOpenEntryStreamInternal(stream, error))
        {
            DebugLogWrite("ERROR", "LAZIP", "STREAM_SEEK_FAIL", "index=%u | target=%llu | reason=reopen_failed | error=%s", (unsigned int)stream->index, (unsigned long long)absolutePosition, error.c_str());
            return false;
        }
    }

    unsigned char temp[64 * 1024];
    uint64_t remaining = absolutePosition - stream->position;

    while (remaining > 0)
    {
        if (stream->cancellation && stream->cancellation->IsCancellationRequested())
        {
            error = "ZIP seek cancelled";
            return false;
        }

        size_t want = sizeof(temp);

        if (remaining < static_cast<uint64_t>(want))
        {
            want = static_cast<size_t>(remaining);
        }

        size_t got = LibarchiveZipReadEntryStream(stream, temp, want, error);

        if (got == 0)
        {
            if (error.empty())
            {
                error = "Unexpected end of ZIP stream while seeking";
            }

            DebugLogWrite("ERROR", "LAZIP", "STREAM_SEEK_FAIL", "index=%u | target=%llu | position=%llu | reason=discard_failed | error=%s", (unsigned int)stream->index, (unsigned long long)absolutePosition, (unsigned long long)stream->position, error.c_str());
            return false;
        }

        remaining -= got;
    }

    return true;
}

// -----------------------------------------------------------------------------
// LibarchiveZipCloseEntryStream
//
// Closes and deletes an opaque libarchive ZIP entry stream.
// -----------------------------------------------------------------------------
void LibarchiveZipCloseEntryStream(void* streamHandle)
{
    LibarchiveZipEntryStream* stream = static_cast<LibarchiveZipEntryStream*>(streamHandle);

    if (!stream)
    {
        return;
    }

    LaZipCloseEntryStreamInternal(stream);
    delete stream;
}



bool LibarchiveZipExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    error.clear();

    if (!archivePath || !archivePath[0] || !outputPath || !outputPath[0])
    {
        error = "Invalid ZIP extract-to-file paths";
        DebugLogWrite("ERROR", "LAZIP", "EXTRACT_FILE_FAIL", "index=%u | reason=invalid_path", (unsigned int)index);
        return false;
    }

    if (_wcsicmp(archivePath, outputPath) == 0)
    {
        error = "ZIP output path must not overwrite the source archive";
        DebugLogWrite("ERROR", "LAZIP", "EXTRACT_FILE_FAIL", "index=%u | reason=output_is_source", (unsigned int)index);
        return false;
    }

    void* zipStream = nullptr;
    uint64_t entrySize = 0;

    if (!LibarchiveZipOpenEntryStream(archivePath, index, &zipStream, &entrySize, error))
        return false;

    HANDLE output = CreateFileW(outputPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (output == INVALID_HANDLE_VALUE)
    {
        DWORD winerr = GetLastError();
        error = "Unable to create ZIP extraction output";
        DebugLogWrite("ERROR", "LAZIP", "EXTRACT_FILE_FAIL", "index=%u | winerr=%lu | reason=create_output_failed", (unsigned int)index, winerr);
        LibarchiveZipCloseEntryStream(zipStream);
        return false;
    }

    bool ok = true;
    uint64_t totalWritten = 0;
    unsigned char buffer[64 * 1024];

    for (;;)
    {
        std::string readError;
        size_t bytesRead = LibarchiveZipReadEntryStream(zipStream, buffer, sizeof(buffer), readError);

        if (!readError.empty())
        {
            error = readError;
            ok = false;
            break;
        }

        if (bytesRead == 0)
            break;

        if (totalWritten > entrySize || bytesRead > entrySize - totalWritten)
        {
            error = "ZIP extraction exceeded the expected entry size";
            ok = false;
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(output, buffer, static_cast<DWORD>(bytesRead), &bytesWritten, nullptr) || bytesWritten != bytesRead)
        {
            DWORD winerr = GetLastError();
            error = "Unable to write ZIP extraction output";
            DebugLogWrite("ERROR", "LAZIP", "EXTRACT_FILE_FAIL", "index=%u | winerr=%lu | position=%llu | reason=write_failed", (unsigned int)index, winerr, (unsigned long long)totalWritten);
            ok = false;
            break;
        }

        totalWritten += bytesWritten;
    }

    if (ok && totalWritten != entrySize)
    {
        error = "ZIP extraction ended before the expected entry size";
        ok = false;
    }

    LibarchiveZipCloseEntryStream(zipStream);

    if (!CloseHandle(output))
    {
        error = "Unable to close ZIP extraction output";
        ok = false;
    }

    if (!ok)
    {
        DebugLogWrite("ERROR", "LAZIP", "EXTRACT_FILE_FAIL", "index=%u | written=%llu | expected=%llu | error=%s", (unsigned int)index, (unsigned long long)totalWritten, (unsigned long long)entrySize, error.c_str());
        DeleteFileW(outputPath);
        return false;
    }

    return true;
}
