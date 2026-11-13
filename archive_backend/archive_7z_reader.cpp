#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <new>
#include <string>
#include <functional>
#include <vector>

#include "archive_common.h"
#include "archive_7z_reader.h"
#include "archive_passwords.h"
#include "archive_7z_stream_reader.h"
#include "debug_log.h"

extern "C" {
    #include "7z.h"
    #include "7zAlloc.h"
    #include "7zCrc.h"
    #include "7zFile.h"
}

static void* SevenZipAlloc(ISzAllocPtr, size_t size)
{
    return std::malloc(size);
}

static void SevenZipFree(ISzAllocPtr, void* address)
{
    std::free(address);
}

static ISzAlloc g_Alloc =
{
    SevenZipAlloc,
    SevenZipFree
};

static ISzAlloc g_AllocTemp =
{
    SevenZipAlloc,
    SevenZipFree
};

static std::string MakeError(const char* message)
{
    return message ? message : "Unknown error";
}

static std::string MakeError(const char* message, SRes res);

static const char* SzErrorToString(SRes res)
{
    switch (res)
    {
    case SZ_OK: return "SZ_OK";
    case SZ_ERROR_DATA: return "SZ_ERROR_DATA";
    case SZ_ERROR_MEM: return "SZ_ERROR_MEM";
    case SZ_ERROR_CRC: return "SZ_ERROR_CRC";
    case SZ_ERROR_UNSUPPORTED: return "SZ_ERROR_UNSUPPORTED";
    case SZ_ERROR_PARAM: return "SZ_ERROR_PARAM";
    case SZ_ERROR_INPUT_EOF: return "SZ_ERROR_INPUT_EOF";
    case SZ_ERROR_OUTPUT_EOF: return "SZ_ERROR_OUTPUT_EOF";
    case SZ_ERROR_READ: return "SZ_ERROR_READ";
    case SZ_ERROR_WRITE: return "SZ_ERROR_WRITE";
    case SZ_ERROR_PROGRESS: return "SZ_ERROR_PROGRESS";
    case SZ_ERROR_FAIL: return "SZ_ERROR_FAIL";
    case SZ_ERROR_THREAD: return "SZ_ERROR_THREAD";
    case SZ_ERROR_ARCHIVE: return "SZ_ERROR_ARCHIVE";
    case SZ_ERROR_NO_ARCHIVE: return "SZ_ERROR_NO_ARCHIVE";
    default: return "UNKNOWN_7ZIP_ERROR";
    }
}

static std::string MakeError(const char* message, SRes res)
{
    std::ostringstream oss;
    oss << message << ": " << SzErrorToString(res) << " (" << static_cast<int>(res) << ")";
    return oss.str();
}

static std::string Utf16ToUtf8(const UInt16* utf16)
{
    if (!utf16)
        return {};

    const wchar_t* wide = reinterpret_cast<const wchar_t*>(utf16);
    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);

    if (needed <= 0)
    {
        DebugLogWrite("WARN", "7Z", "UTF16_TO_UTF8_FAIL", "reason=size_query_failed | winerr=%lu", GetLastError());
        return {};
    }

    std::string result;
    result.resize(static_cast<size_t>(needed - 1));

    int converted = WideCharToMultiByte(CP_UTF8, 0, wide, -1, result.data(), needed, nullptr, nullptr);

    if (converted <= 0)
    {
        DebugLogWrite("WARN", "7Z", "UTF16_TO_UTF8_FAIL", "reason=convert_failed | winerr=%lu", GetLastError());
        return {};
    }

    return result;
}
// -----------------------------------------------------------------------------
// 7z split-volume input stream support
//
// 7-Zip split archives commonly appear as:
//     album.7z.001
//     album.7z.002
//     album.7z.003
//
// The 7z SDK parser expects one logical seekable byte stream. Opening only the
// .001 file causes SzArEx_Open() to hit EOF before the archive header/data is
// complete. These helpers expose the numbered parts as one concatenated
// ISeekInStream so normal list/extract/stream code can keep using SzArEx_Open().
// -----------------------------------------------------------------------------
struct SevenZipVolumePart
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    UInt64 size = 0;
    std::wstring path;
};

struct SevenZipConcatInStream
{
    ISeekInStream vt{};
    std::vector<SevenZipVolumePart> parts;
    UInt64 totalSize = 0;
    UInt64 position = 0;
};

// -----------------------------------------------------------------------------
// SevenZipConcatFromStream
//
// Converts the ISeekInStream callback pointer back to the owning concat-stream
// object.
//
// The 7z SDK calls our Read/Seek callbacks through the ISeekInStream vtable.
// Because SevenZipConcatInStream stores that vtable as its first struct member,
// the vtable pointer has the same address as the owning object. This replaces the
// CONTAINER_FROM_VTBL helper macro used by some SDK samples, which is not defined
// in this project.
// -----------------------------------------------------------------------------
static SevenZipConcatInStream* SevenZipConcatFromStream(const ISeekInStream* stream)
{
    return reinterpret_cast<SevenZipConcatInStream*>(const_cast<ISeekInStream*>(stream));
}

static bool WideEndsWithInsensitive(const wchar_t* value, const wchar_t* suffix)
{
    if (!value || !suffix)
    {
        return false;
    }

    size_t valueLen = wcslen(value);
    size_t suffixLen = wcslen(suffix);

    if (valueLen < suffixLen)
    {
        return false;
    }

    return _wcsicmp(value + valueLen - suffixLen, suffix) == 0;
}

static bool IsSevenZipSplitFirstVolumePathW(const wchar_t* archivePath)
{
    return WideEndsWithInsensitive(archivePath, L".7z.001");
}

static bool BuildSevenZipSplitVolumePath(const std::wstring& firstVolumePath, unsigned int volumeNumber, std::wstring& outPath)
{
    outPath.clear();

    if (firstVolumePath.length() < 3)
    {
        DebugLogWrite("ERROR", "7Z", "SPLIT_PATH_FAIL", "first_volume=%ls | volume=%u | reason=path_too_short", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    if (volumeNumber == 0 || volumeNumber > 999)
    {
        DebugLogWrite("ERROR", "7Z", "SPLIT_PATH_FAIL", "first_volume=%ls | volume=%u | reason=invalid_volume_number", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    wchar_t suffix[8];
    swprintf_s(suffix, L"%03u", volumeNumber);

    outPath = firstVolumePath.substr(0, firstVolumePath.length() - 3);
    outPath += suffix;

    return true;
}

static SRes SevenZipConcatRead(const ISeekInStream* stream, void* buffer, size_t* size)
{
    if (!stream || !buffer || !size)
    {
        return SZ_ERROR_PARAM;
    }

    SevenZipConcatInStream* concat = SevenZipConcatFromStream(stream);
    size_t requested = *size;
    size_t totalRead = 0;
    Byte* out = static_cast<Byte*>(buffer);

    while (totalRead < requested && concat->position < concat->totalSize)
    {
        UInt64 partBase = 0;
        size_t partIndex = 0;
        bool foundPart = false;

        for (size_t i = 0; i < concat->parts.size(); ++i)
        {
            UInt64 partEnd = partBase + concat->parts[i].size;

            if (concat->position >= partBase && concat->position < partEnd)
            {
                partIndex = i;
                foundPart = true;
                break;
            }

            partBase = partEnd;
        }

        if (!foundPart)
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "SPLIT_READ_FAIL", "position=%llu | total_size=%llu | reason=part_not_found", (unsigned long long)concat->position, (unsigned long long)concat->totalSize);
            return SZ_ERROR_READ;
        }

        SevenZipVolumePart& part = concat->parts[partIndex];

        if (part.handle == INVALID_HANDLE_VALUE)
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "SPLIT_READ_FAIL", "part=%zu | path=%ls | reason=invalid_handle", partIndex, part.path.c_str());
            return SZ_ERROR_READ;
        }

        UInt64 partOffset64 = concat->position - partBase;
        UInt64 partRemaining64 = part.size - partOffset64;
        size_t want = requested - totalRead;

        if (partRemaining64 < static_cast<UInt64>(want))
        {
            want = static_cast<size_t>(partRemaining64);
        }

        LARGE_INTEGER seekPos;
        seekPos.QuadPart = static_cast<LONGLONG>(partOffset64);

        if (!SetFilePointerEx(part.handle, seekPos, NULL, FILE_BEGIN))
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "SPLIT_READ_FAIL", "part=%zu | path=%ls | offset=%llu | reason=seek_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, GetLastError());
            return SZ_ERROR_READ;
        }

        DWORD bytesRead = 0;

        if (!ReadFile(part.handle, out + totalRead, static_cast<DWORD>(want), &bytesRead, NULL))
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "SPLIT_READ_FAIL", "part=%zu | path=%ls | offset=%llu | requested=%zu | reason=read_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, want, GetLastError());
            return SZ_ERROR_READ;
        }

        if (bytesRead == 0)
        {
            break;
        }

        totalRead += bytesRead;
        concat->position += bytesRead;
    }

    *size = totalRead;
    return SZ_OK;
}

static SRes SevenZipConcatSeek(const ISeekInStream* stream, Int64* pos, ESzSeek origin)
{
    if (!stream || !pos)
    {
        return SZ_ERROR_PARAM;
    }

    SevenZipConcatInStream* concat = SevenZipConcatFromStream(stream);
    Int64 newPos = 0;

    if (origin == SZ_SEEK_SET)
    {
        newPos = *pos;
    }
    else if (origin == SZ_SEEK_CUR)
    {
        newPos = static_cast<Int64>(concat->position) + *pos;
    }
    else if (origin == SZ_SEEK_END)
    {
        newPos = static_cast<Int64>(concat->totalSize) + *pos;
    }
    else
    {
        DebugLogWrite("ERROR", "7Z", "SPLIT_SEEK_FAIL", "origin=%d | reason=unsupported_origin", (int)origin);
        return SZ_ERROR_PARAM;
    }

    if (newPos < 0)
    {
        DebugLogWrite("ERROR", "7Z", "SPLIT_SEEK_FAIL", "target=%lld | reason=negative_position", (long long)newPos);
        return SZ_ERROR_PARAM;
    }

    concat->position = static_cast<UInt64>(newPos);
    *pos = newPos;

    return SZ_OK;
}

static void SevenZipConcatStreamInit(SevenZipConcatInStream* stream)
{
    if (!stream)
    {
        return;
    }

    stream->vt.Read = SevenZipConcatRead;
    stream->vt.Seek = SevenZipConcatSeek;
    stream->totalSize = 0;
    stream->position = 0;
    stream->parts.clear();
}

static void SevenZipConcatStreamClose(SevenZipConcatInStream* stream)
{
    if (!stream)
    {
        return;
    }

    for (SevenZipVolumePart& part : stream->parts)
    {
        if (part.handle != INVALID_HANDLE_VALUE)
        {
            CloseHandle(part.handle);
            part.handle = INVALID_HANDLE_VALUE;
        }
    }

    stream->parts.clear();
    stream->totalSize = 0;
    stream->position = 0;
}

static bool SevenZipConcatStreamOpen(SevenZipConcatInStream* stream, const wchar_t* firstVolumePath, std::string& error)
{
    if (!stream || !firstVolumePath || !*firstVolumePath)
    {
        error = "7z split stream open failed: invalid input";
        DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "reason=invalid_input");
        return false;
    }

    SevenZipConcatStreamClose(stream);
    SevenZipConcatStreamInit(stream);

    std::wstring firstPath(firstVolumePath);
    bool openedAny = false;

    for (unsigned int volumeNumber = 1; volumeNumber <= 999; ++volumeNumber)
    {
        std::wstring partPath;

        if (!BuildSevenZipSplitVolumePath(firstPath, volumeNumber, partPath))
        {
            error = "7z split stream open failed: failed to build volume path";
            SevenZipConcatStreamClose(stream);
            return false;
        }

        DWORD attrs = GetFileAttributesW(partPath.c_str());

        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            if (!openedAny)
            {
                error = "7z split stream open failed: first volume does not exist";
                DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "path=%ls | reason=first_volume_missing | winerr=%lu", partPath.c_str(), GetLastError());
                SevenZipConcatStreamClose(stream);
                return false;
            }

            DebugLogWrite("INFO", "7Z", "SPLIT_OPEN_DONE", "volumes=%zu | total_size=%llu | missing_next=%ls", stream->parts.size(), (unsigned long long)stream->totalSize, partPath.c_str());
            break;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            error = "7z split stream open failed: volume path is a directory";
            DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "path=%ls | reason=volume_is_directory", partPath.c_str());
            SevenZipConcatStreamClose(stream);
            return false;
        }

        HANDLE handle = CreateFileW(partPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "7z split stream open failed: failed to open volume";
            DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "path=%ls | reason=open_volume_failed | winerr=%lu", partPath.c_str(), GetLastError());
            SevenZipConcatStreamClose(stream);
            return false;
        }

        LARGE_INTEGER fileSize;

        if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0)
        {
            error = "7z split stream open failed: failed to read volume size";
            DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "path=%ls | reason=get_size_failed | winerr=%lu", partPath.c_str(), GetLastError());
            CloseHandle(handle);
            SevenZipConcatStreamClose(stream);
            return false;
        }

        SevenZipVolumePart part;
        part.handle = handle;
        part.size = static_cast<UInt64>(fileSize.QuadPart);
        part.path = partPath;

        try
        {
            stream->parts.push_back(part);
        }
        catch (const std::bad_alloc&)
        {
            error = "7z split stream open failed: not enough memory for volume list";
            DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "path=%ls | reason=volume_list_alloc_failed", partPath.c_str());
            CloseHandle(handle);
            SevenZipConcatStreamClose(stream);
            return false;
        }

        stream->totalSize += part.size;
        openedAny = true;

        //DebugLogWrite("INFO", "7Z", "SPLIT_OPEN_PART", "volume=%u | path=%ls | size=%llu | total_size=%llu", volumeNumber, partPath.c_str(), (unsigned long long)part.size, (unsigned long long)stream->totalSize);
    }

    if (stream->parts.empty() || stream->totalSize == 0)
    {
        error = "7z split stream open failed: no usable volume data";
        DebugLogWrite("ERROR", "7Z", "SPLIT_OPEN_FAIL", "reason=no_volume_data");
        SevenZipConcatStreamClose(stream);
        return false;
    }

    return true;
}

// The SDK receives passwords only during the synchronous Open/Extract call.
// Restore the caller's thread-local value before returning (including on error).
class SevenZipScopedPassword
{
public:
    explicit SevenZipScopedPassword(const std::string& utf8)
    {
        if (!utf8.empty())
        {
            int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, nullptr, 0);
            if (size > 0)
            {
                wide_.resize(static_cast<size_t>(size));
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), -1, wide_.data(), size);
            }
        }
        previous_ = Sz7z_SetPassword(wide_.empty() ? nullptr : reinterpret_cast<const UInt16*>(wide_.c_str()));
    }

    ~SevenZipScopedPassword()
    {
        Sz7z_SetPassword(previous_);
        if (!wide_.empty())
            SecureZeroMemory(wide_.data(), wide_.size() * sizeof(wchar_t));
    }

    SevenZipScopedPassword(const SevenZipScopedPassword&) = delete;
    SevenZipScopedPassword& operator=(const SevenZipScopedPassword&) = delete;
private:
    std::wstring wide_;
    const UInt16* previous_ = nullptr;
};

struct SevenZipPasswordCandidate
{
    std::string value;
    bool save = false;
};

static std::string SevenZipPasswordKey(const wchar_t* archivePath)
{
    if (!archivePath)
        return {};
    int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, archivePath, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
        return {};
    std::string key(static_cast<size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, archivePath, -1, key.data(), bytes, nullptr, nullptr) != bytes)
        return {};
    key.pop_back();
    return key;
}

static bool SevenZipWithPasswords(const wchar_t* path, const std::string& entry,
    const std::function<bool(const std::string&, std::string&)>& tryCandidate, std::string& error, bool streamMode = false)
{
    // Unsupported stream graphs and solid folders cannot be repaired by a
    // password; do not show spurious password dialogs before memory fallback.
    auto deterministicStreamFailure = [&](const std::string& message)
    {
        if (!streamMode)
            return false;
        if (message.rfind("7z stream open failed: SzArEx_Open failed", 0) == 0)
            return false;
        return message.rfind("7z stream open failed:", 0) == 0 ||
            message.rfind("7z stream split open failed:", 0) == 0 ||
            message.rfind("7z AES stream unsupported coder graph:", 0) == 0 ||
            message.rfind("7z AES stream has an invalid decrypted packed size", 0) == 0;
    };

    const std::string key = SevenZipPasswordKey(path);
    // Unencrypted 7z remains a single SDK attempt without password-list I/O.
    std::string lastError;
    if (tryCandidate("", lastError))
    {
        error.clear();
        return true;
    }
    if (deterministicStreamFailure(lastError))
    {
        error = lastError;
        return false;
    }
    std::vector<SevenZipPasswordCandidate> candidates;
    auto addCandidate = [&](const std::string& value)
    {
        if (value.empty())
            return;
        for (const auto& candidate : candidates)
            if (candidate.value == value)
                return;
        candidates.push_back({value, false});
    };
    std::vector<std::string> runtime;
    ArchivePasswordGetRuntimePasswords(key.c_str(), runtime);
    for (const auto& candidate : runtime)
        addCandidate(candidate);
    ArchivePasswordList saved;
    std::string listError;
    if (!ArchivePasswordLoadList(saved, listError))
        DebugLogWrite("WARN", "7Z", "PASSWORD_LIST_FAIL", "error=%s", listError.c_str());
    for (const auto& candidate : saved.passwords)
        addCandidate(candidate);

    for (const auto& candidate : candidates)
    {
        lastError.clear();
        if (tryCandidate(candidate.value, lastError))
        {
            if (!candidate.value.empty())
            {
                ArchivePasswordAddRuntimePassword(key.c_str(), candidate.value.c_str());
                ArchivePasswordNotifyAccepted(key.c_str());
            }
            error.clear();
            return true;
        }
        if (deterministicStreamFailure(lastError))
        {
            error = lastError;
            return false;
        }
    }
    if (!ArchivePasswordIsArchiveCancelled(key.c_str()))
    {
        char buffer[256]{};
        bool savePassword = false;
        if (ArchivePasswordPromptUser(key.c_str(), entry.c_str(), buffer, sizeof(buffer), &savePassword))
        {
            const std::string entered = buffer;
            SecureZeroMemory(buffer, sizeof(buffer));
            if (!entered.empty() && tryCandidate(entered, lastError))
            {
                ArchivePasswordAddRuntimePassword(key.c_str(), entered.c_str());
                ArchivePasswordNotifyAccepted(key.c_str());
                if (savePassword)
                {
                    std::string saveError;
                    if (!ArchivePasswordAppendIfMissing(entered.c_str(), saveError))
                        DebugLogWrite("WARN", "7Z", "PASSWORD_SAVE_FAIL", "error=%s", saveError.c_str());
                }
                error.clear();
                return true;
            }
        }
        SecureZeroMemory(buffer, sizeof(buffer));
    }
    error = lastError.empty() ? "7z archive password missing or incorrect" : lastError;
    return false;
}

struct SevenZipOpenArchive
{
    CFileInStream fileStream{};

    SevenZipConcatInStream concatStream{};

    CLookToRead2 lookStream{};

    CSzArEx db{};

    bool fileOpen = false;

    bool concatOpen = false;

    bool dbOpen = false;

    SevenZipOpenArchive()
    {
        FileInStream_CreateVTable(&fileStream);
        SevenZipConcatStreamInit(&concatStream);
        LookToRead2_CreateVTable(&lookStream, False);
        SzArEx_Init(&db);
    }

    ~SevenZipOpenArchive()
    {
        close();
    }

    // -------------------------------------------------------------------------
    // SevenZipOpenArchive::open
    //
    // Opens a normal 7z archive or a first-volume split 7z archive.
    //
    // Normal .7z paths use the SDK CFileInStream directly. Split .7z.001 paths
    // use SevenZipConcatInStream so SzArEx_Open() sees all numbered parts as one
    // logical seekable stream. Without this, multipart 7z listing fails with
    // SZ_ERROR_INPUT_EOF after reading only the .001 file.
    //
    // All open/setup failures are logged because they block archive expansion and
    // playback routing.
    // -------------------------------------------------------------------------
    bool open(const wchar_t* archivePath, std::string& error, const std::string& password = {})
    {
        if (!archivePath || !*archivePath)
        {
            error = MakeError("Archive path is empty");
            DebugLogWrite("ERROR", "7Z", "OPEN_FAIL", "reason=empty_path");
            return false;
        }

        bool useSplitStream = IsSevenZipSplitFirstVolumePathW(archivePath);

        if (useSplitStream)
        {
            if (!SevenZipConcatStreamOpen(&concatStream, archivePath, error))
            {
                DebugLogWrite("ERROR", "7Z", "OPEN_FAIL", "path=%ls | reason=split_stream_open_failed | error=%s", archivePath, error.c_str());
                return false;
            }

            concatOpen = true;
            lookStream.realStream = &concatStream.vt;

            DebugLogWrite("INFO", "7Z", "OPEN_SPLIT_STREAM", "path=%ls | volumes=%zu | total_size=%llu", archivePath, concatStream.parts.size(), (unsigned long long)concatStream.totalSize);
        }
        else
        {
            if (InFile_OpenW(&fileStream.file, archivePath) != 0)
            {
                error = MakeError("Failed to open archive file");
                DebugLogWrite("ERROR", "7Z", "OPEN_FAIL", "path=%ls | reason=open_file_failed", archivePath);
                return false;
            }

            fileOpen = true;
            lookStream.realStream = &fileStream.vt;
        }

        lookStream.bufSize = 1 << 18; // 256 KiB
        lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&g_Alloc, lookStream.bufSize));

        if (!lookStream.buf)
        {
            error = MakeError("Failed to allocate 7z input buffer");
            DebugLogWrite("ERROR", "7Z", "OPEN_FAIL", "path=%ls | bytes=%zu | reason=input_buffer_alloc_failed", archivePath, lookStream.bufSize);
            return false;
        }

        LookToRead2_INIT(&lookStream);
        CrcGenerateTable();

        SevenZipScopedPassword scope(password);
        SRes res = SzArEx_Open(&db, &lookStream.vt, &g_Alloc, &g_AllocTemp);

        if (res != SZ_OK)
        {
            error = MakeError("SzArEx_Open failed", res);
            DebugLogWrite("ERROR", "7Z", "OPEN_FAIL", "path=%ls | split=%d | res=%d | error=%s", archivePath, useSplitStream ? 1 : 0, (int)res, error.c_str());
            return false;
        }

        dbOpen = true;

        DebugLogWrite("INFO", "7Z", "OPEN_OK", "path=%ls | split=%d | files=%u | folders=%u", archivePath, useSplitStream ? 1 : 0, (unsigned int)db.NumFiles, (unsigned int)db.db.NumFolders);

        return true;
    }

    // -------------------------------------------------------------------------
    // SevenZipOpenArchive::close
    //
    // Releases 7z database state, input buffers, and either the normal file stream
    // or all split-volume file handles. This keeps normal .7z and .7z.001 cleanup
    // in one place.
    // -------------------------------------------------------------------------
    void close()
    {
        if (dbOpen)
        {
            SzArEx_Free(&db, &g_Alloc);
            dbOpen = false;
        }

        if (lookStream.buf)
        {
            ISzAlloc_Free(&g_Alloc, lookStream.buf);
            lookStream.buf = nullptr;
            lookStream.bufSize = 0;
        }

        if (concatOpen)
        {
            SevenZipConcatStreamClose(&concatStream);
            concatOpen = false;
        }

        if (fileOpen)
        {
            File_Close(&fileStream.file);
            fileOpen = false;
        }
    }
};

static uint32_t CountRealFilesInSevenZipFolder(const CSzArEx* db, UInt32 folderIndex)
{
    if (!db)
        return 0;

    if (folderIndex == 0xFFFFFFFF)
        return 0;

    if (folderIndex >= db->db.NumFolders)
        return 0;

    UInt32 startFile = db->FolderToFile[folderIndex];
    UInt32 endFile = db->FolderToFile[folderIndex + 1];

    uint32_t count = 0;

    for (UInt32 i = startFile; i < endFile && i < db->NumFiles; ++i)
    {
        if (!SzArEx_IsDir(db, i))
            ++count;
    }

    return count;
}

static bool GetSevenZipFolderPrimaryMethod(const CSzArEx* db, UInt32 folderIndex, uint32_t* outMethodId)
{
    if (outMethodId)
        *outMethodId = 0;

    if (!db || !outMethodId)
        return false;

    if (folderIndex == 0xFFFFFFFF)
        return false;

    if (folderIndex >= db->db.NumFolders)
        return false;

    CSzFolder folder;
    CSzData sd;
    const Byte* data = db->db.CodersData + db->db.FoCodersOffsets[folderIndex];

    sd.Data = data;
    sd.Size = db->db.FoCodersOffsets[(size_t)folderIndex + 1] - db->db.FoCodersOffsets[folderIndex];

    SRes res = SzGetNextFolderItem(&folder, &sd);

    if (res != SZ_OK)
        return false;

    if (sd.Size != 0)
        return false;

    if (folder.NumCoders < 1)
        return false;

    const CSzCoderInfo* coder = &folder.Coders[0];

    if (coder->MethodID > 0xFFFFFFFF)
        return false;

    *outMethodId = static_cast<uint32_t>(coder->MethodID);
    return true;
}

static bool SevenZipListEntriesCore(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error, const std::string& password)
{
    entries.clear();
    error.clear();

    SevenZipOpenArchive archive;

    if (!archive.open(archivePath, error, password))
    {
        DebugLogWrite("ERROR", "7Z", "LIST_FAIL", "reason=open_failed | error=%s", error.c_str());
        return false;
    }

    try
    {
        entries.reserve(archive.db.NumFiles);
    }
    catch (const std::bad_alloc&)
    {
        error = "Not enough memory to reserve 7z entry list";
        DebugLogWrite("ERROR", "7Z", "LIST_FAIL", "reason=reserve_failed | files=%u | error=%s", (unsigned int)archive.db.NumFiles, error.c_str());
        return false;
    }

    for (UInt32 i = 0; i < archive.db.NumFiles; ++i)
    {
        size_t nameLen = SzArEx_GetFileNameUtf16(&archive.db, i, nullptr);

        std::vector<UInt16> nameBuf;

        try
        {
            nameBuf.resize(nameLen);
        }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory to read 7z entry name";
            DebugLogWrite("ERROR", "7Z", "LIST_FAIL", "reason=name_alloc_failed | index=%u | name_len=%zu | error=%s", (unsigned int)i, nameLen, error.c_str());
            return false;
        }

        SzArEx_GetFileNameUtf16(&archive.db, i, nameBuf.data());

        std::string path = Utf16ToUtf8(nameBuf.data());

        if (path.empty() && nameLen > 0)
            DebugLogWrite("WARN", "7Z", "LIST_ENTRY_WARN", "index=%u | reason=empty_utf8_path", (unsigned int)i);

        ArchiveEntry entry;
        entry.index = static_cast<uint32_t>(i);
        entry.path = path;
        entry.size = static_cast<uint64_t>(SzArEx_GetFileSize(&archive.db, i));
        entry.isFolder = SzArEx_IsDir(&archive.db, i) != 0;
        entry.isSupportedAudio = !entry.isFolder && ArchiveIsSupportedAudioPath(path);
        entry.hasCrc32 = SzBitWithVals_Check(&archive.db.CRCs, i) != 0;
        entry.crc32 = entry.hasCrc32 ? static_cast<uint32_t>(archive.db.CRCs.Vals[i]) : 0;

        entry.folderIndex = 0xFFFFFFFF;
        entry.filesInFolder = 0;
        entry.isSolid = false;

        if (!entry.isFolder && archive.db.FileToFolder)
        {
            UInt32 folderIndex = archive.db.FileToFolder[i];
            entry.folderIndex = static_cast<uint32_t>(folderIndex);
            entry.filesInFolder = CountRealFilesInSevenZipFolder(&archive.db, folderIndex);
            entry.isSolid = entry.filesInFolder > 1;

            uint32_t methodId = 0;

            if (GetSevenZipFolderPrimaryMethod(&archive.db, folderIndex, &methodId))
            {
                entry.methodId = methodId;
                entry.hasMethodId = true;

                /*
                    Current true-streaming support:
                    Copy/store only.
                    0 = k_Copy in 7z SDK.
                    LZMA/LZMA2 will be added later.
                */
                entry.streamCandidate =
                    !entry.isSolid &&
                    entry.filesInFolder == 1 &&
                    (
                        methodId == 0 ||        // Copy/store
                        methodId == 0x21        // LZMA2
                        );
            }
        }

        if (!entry.isFolder && entry.folderIndex < archive.db.db.NumFolders)
        {
            CSzFolder folder{};
            CSzData props{};
            props.Data = archive.db.db.CodersData + archive.db.db.FoCodersOffsets[entry.folderIndex];
            props.Size = archive.db.db.FoCodersOffsets[(size_t)entry.folderIndex + 1] - archive.db.db.FoCodersOffsets[entry.folderIndex];
            if (SzGetNextFolderItem(&folder, &props) == SZ_OK && props.Size == 0)
            {
                for (UInt32 coder = 0; coder < folder.NumCoders; ++coder)
                {
                    if (folder.Coders[coder].MethodID == 0x06F10701)
                    {
                        entry.isEncrypted = true;
                        entry.streamCandidate = !entry.isSolid && entry.filesInFolder == 1 &&
                            (folder.NumCoders == 2 || folder.NumCoders == 3) &&
                            folder.NumBonds == folder.NumCoders - 1 &&
                            folder.Bonds[0].InIndex == 1 && folder.Bonds[0].OutIndex == 0 &&
                            folder.NumPackStreams == 1 && folder.PackStreams[0] == 0 &&
                            folder.UnpackStream == folder.NumCoders - 1 && folder.Coders[1].MethodID == 0x21 &&
                            (folder.NumCoders == 2 || (folder.Bonds[1].InIndex == 2 &&
                                folder.Bonds[1].OutIndex == 1 && folder.Coders[2].MethodID == 0x03 &&
                                folder.Coders[2].PropsSize == 1));
                        break;
                    }
                }
            }
        }

        try
        {
            entries.push_back(entry);
        }
        catch (const std::bad_alloc&)
        {
            error = "Not enough memory to add 7z entry";
            DebugLogWrite("ERROR", "7Z", "LIST_FAIL", "reason=push_entry_failed | index=%u | error=%s", (unsigned int)i, error.c_str());
            return false;
        }
    }

    return true;
}

bool SevenZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    return SevenZipWithPasswords(archivePath, "", [&](const std::string& password, std::string& attemptError)
    {
        return SevenZipListEntriesCore(archivePath, entries, attemptError, password);
    }, error);
}

static bool WriteBufferToFileW(const wchar_t* outputPath, const void* data, size_t dataSize, std::string& error)
{
    if (!outputPath || !*outputPath)
    {
        error = "Output path is empty";
        DebugLogWrite("ERROR", "7Z", "WRITE_FILE_FAIL", "bytes=%zu | reason=empty_output_path", dataSize);
        return false;
    }

    HANDLE hFile = CreateFileW(outputPath, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

    if (hFile == INVALID_HANDLE_VALUE)
    {
        error = "Failed to create output cache file";
        DebugLogWrite("ERROR", "7Z", "WRITE_FILE_FAIL", "bytes=%zu | winerr=%lu | reason=create_file_failed", dataSize, GetLastError());
        return false;
    }

    const unsigned char* p = static_cast<const unsigned char*>(data);
    size_t remaining = dataSize;
    bool ok = true;

    while (remaining > 0)
    {
        DWORD chunk = remaining > 1024 * 1024 ? 1024 * 1024 : static_cast<DWORD>(remaining);
        DWORD written = 0;

        if (!WriteFile(hFile, p, chunk, &written, NULL) || written != chunk)
        {
            DebugLogWrite("ERROR", "7Z", "WRITE_FILE_FAIL", "requested=%lu | written=%lu | remaining=%zu | winerr=%lu | reason=write_chunk_failed", chunk, written, remaining, GetLastError());
            ok = false;
            break;
        }

        p += written;
        remaining -= written;
    }

    CloseHandle(hFile);

    if (!ok)
    {
        DeleteFileW(outputPath);
        error = "Failed while writing output cache file";
        return false;
    }

    return true;
}

static bool SevenZipExtractEntryToMemoryCore(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error, const std::string& password)
{
    data.clear();
    error.clear();

    SevenZipOpenArchive archive;

    if (!archive.open(archivePath, error, password))
    {
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | reason=open_failed | error=%s", (unsigned int)index, error.c_str());
        return false;
    }

    if (index >= archive.db.NumFiles)
    {
        std::ostringstream oss;
        oss << "Extract failed: index " << index << " is out of range. Archive contains " << archive.db.NumFiles << " item(s).";
        error = oss.str();
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | files=%u | reason=index_out_of_range", (unsigned int)index, (unsigned int)archive.db.NumFiles);
        return false;
    }

    if (SzArEx_IsDir(&archive.db, index))
    {
        error = "Extract failed: selected index is a folder";
        DebugLogWrite("WARN", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | reason=index_is_folder", (unsigned int)index);
        return false;
    }

    UInt64 expectedSize64 = SzArEx_GetFileSize(&archive.db, index);

    if (expectedSize64 > static_cast<UInt64>(SIZE_MAX))
    {
        error = "Extract failed: file is too large for this 32-bit process";
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | bytes=%llu | reason=too_large_for_process", (unsigned int)index, (unsigned long long)expectedSize64);
        return false;
    }

    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    size_t offset = 0;
    size_t outSizeProcessed = 0;

    SevenZipScopedPassword scope(password);
    SRes res = SzArEx_Extract(&archive.db, &archive.lookStream.vt, index, &blockIndex, &outBuffer, &outBufferSize, &offset, &outSizeProcessed, &g_Alloc, &g_AllocTemp);

    if (res != SZ_OK)
    {
        error = MakeError("SzArEx_Extract failed", res);
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | res=%d | error=%s", (unsigned int)index, (int)res, error.c_str());

        if (outBuffer)
            ISzAlloc_Free(&g_Alloc, outBuffer);

        return false;
    }

    if (!outBuffer && outSizeProcessed > 0)
    {
        error = "Extract failed: decoder returned no output buffer";
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | processed=%zu | reason=no_output_buffer", (unsigned int)index, outSizeProcessed);
        return false;
    }

    const Byte* fileData = outBuffer + offset;

    if (expectedSize64 > ARCHIVE_MAX_EXTRACT_TO_MEMORY_BYTES)
    {
        error = "7z entry is too large to extract to memory";
        DebugLogWrite("WARN", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | bytes=%llu | limit=%llu | reason=entry_too_large_for_memory_route", (unsigned int)index, (unsigned long long)expectedSize64, (unsigned long long)ARCHIVE_MAX_EXTRACT_TO_MEMORY_BYTES);

        if (outBuffer)
            ISzAlloc_Free(&g_Alloc, outBuffer);

        return false;
    }

    try
    {
        data.resize(outSizeProcessed);
    }
    catch (const std::bad_alloc&)
    {
        error = "Not enough memory to copy extracted 7z entry";
        DebugLogWrite("ERROR", "7Z", "MEMORY_EXTRACT_FAIL", "index=%u | bytes=%zu | reason=copy_alloc_failed", (unsigned int)index, outSizeProcessed);

        if (outBuffer)
            ISzAlloc_Free(&g_Alloc, outBuffer);

        return false;
    }

    if (outSizeProcessed > 0)
        std::memcpy(data.data(), fileData, outSizeProcessed);

    if (outBuffer)
        ISzAlloc_Free(&g_Alloc, outBuffer);

    return true;
}

bool SevenZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    return SevenZipWithPasswords(archivePath, std::to_string(index), [&](const std::string& password, std::string& attemptError)
    {
        return SevenZipExtractEntryToMemoryCore(archivePath, index, data, attemptError, password);
    }, error);
}

static bool SevenZipExtractEntryToFileCore(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error, const std::string& password)
{
    error.clear();

    if (!outputPath || !*outputPath)
    {
        error = "Output path is empty";
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | reason=empty_output_path", (unsigned int)index);
        return false;
    }

    SevenZipOpenArchive archive;

    if (!archive.open(archivePath, error, password))
    {
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | reason=open_failed | error=%s", (unsigned int)index, error.c_str());
        return false;
    }

    if (index >= archive.db.NumFiles)
    {
        std::ostringstream oss;
        oss << "Extract-to-file failed: index " << index << " is out of range. Archive contains " << archive.db.NumFiles << " item(s).";
        error = oss.str();
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | files=%u | reason=index_out_of_range", (unsigned int)index, (unsigned int)archive.db.NumFiles);
        return false;
    }

    if (SzArEx_IsDir(&archive.db, index))
    {
        error = "Extract-to-file failed: selected index is a folder";
        DebugLogWrite("WARN", "7Z", "FILE_EXTRACT_FAIL", "index=%u | reason=index_is_folder", (unsigned int)index);
        return false;
    }

    UInt64 expectedSize64 = SzArEx_GetFileSize(&archive.db, index);

    if (expectedSize64 > static_cast<UInt64>(SIZE_MAX))
    {
        error = "Extract-to-file failed: file is too large for this 32-bit process";
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | bytes=%llu | reason=too_large_for_process", (unsigned int)index, (unsigned long long)expectedSize64);
        return false;
    }

    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    size_t offset = 0;
    size_t outSizeProcessed = 0;

    SevenZipScopedPassword scope(password);
    SRes res = SzArEx_Extract(&archive.db, &archive.lookStream.vt, index, &blockIndex, &outBuffer, &outBufferSize, &offset, &outSizeProcessed, &g_Alloc, &g_AllocTemp);

    if (res != SZ_OK)
    {
        error = MakeError("SzArEx_Extract failed during extract-to-file", res);
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | res=%d | error=%s", (unsigned int)index, (int)res, error.c_str());

        if (outBuffer)
            ISzAlloc_Free(&g_Alloc, outBuffer);

        return false;
    }

    if (!outBuffer && outSizeProcessed > 0)
    {
        error = "Extract-to-file failed: decoder returned no output buffer";
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | processed=%zu | reason=no_output_buffer", (unsigned int)index, outSizeProcessed);
        return false;
    }

    const Byte* fileData = outBuffer + offset;
    bool ok = WriteBufferToFileW(outputPath, fileData, outSizeProcessed, error);

    if (outBuffer)
        ISzAlloc_Free(&g_Alloc, outBuffer);

    if (!ok)
    {
        DebugLogWrite("ERROR", "7Z", "FILE_EXTRACT_FAIL", "index=%u | bytes=%zu | reason=write_output_failed | error=%s", (unsigned int)index, outSizeProcessed, error.c_str());
        return false;
    }

    return true;
}

// -----------------------------------------------------------------------------
// SevenZipExtractEntriesToFiles
//
// Extracts several 7z entries while keeping one archive session open and reusing
// the 7z SDK block buffer across calls to SzArEx_Extract().
//
// This is mainly for solid 7z cache warming. For entries that live in the same
// solid block/folder, the first extraction decodes the block and later entries can
// be written from slices of the already decoded output buffer instead of opening
// the archive and decoding the same solid block again for every track.
//
// Returns true if at least one requested entry was successfully written. Each
// failed entry is logged and skipped so one bad item does not stop the rest of
// the cache-warm pass.
// -----------------------------------------------------------------------------
bool SevenZipExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    return SevenZipWithPasswords(archivePath, std::to_string(index), [&](const std::string& password, std::string& attemptError)
    {
        return SevenZipExtractEntryToFileCore(archivePath, index, outputPath, attemptError, password);
    }, error);
}

static bool SevenZipExtractEntriesToFilesCore(const wchar_t* archivePath, const std::vector<SevenZipExtractFileRequest>& requests, std::vector<uint32_t>& outSucceededIndexes, std::string& error, const std::string& password)
{
    outSucceededIndexes.clear();
    error.clear();

    if (!archivePath || !*archivePath)
    {
        error = "Archive path is empty";
        DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_FAIL", "reason=empty_archive_path");
        return false;
    }

    if (requests.empty())
    {
        error = "No 7z batch extraction requests were provided";
        DebugLogWrite("WARN", "7Z", "BATCH_FILE_EXTRACT_SKIP", "reason=no_requests");
        return false;
    }

    SevenZipOpenArchive archive;

    if (!archive.open(archivePath, error, password))
    {
        DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_FAIL", "requests=%zu | reason=open_failed | error=%s", requests.size(), error.c_str());
        return false;
    }

    std::vector<SevenZipExtractFileRequest> orderedRequests = requests;

    std::sort(
        orderedRequests.begin(),
        orderedRequests.end(),
        [&archive](const SevenZipExtractFileRequest& a, const SevenZipExtractFileRequest& b)
        {
            UInt32 folderA = 0xFFFFFFFF;
            UInt32 folderB = 0xFFFFFFFF;

            if (archive.db.FileToFolder)
            {
                if (a.index < archive.db.NumFiles)
                {
                    folderA = archive.db.FileToFolder[a.index];
                }

                if (b.index < archive.db.NumFiles)
                {
                    folderB = archive.db.FileToFolder[b.index];
                }
            }

            if (folderA != folderB)
            {
                return folderA < folderB;
            }

            return a.index < b.index;
        }
    );

    UInt32 blockIndex = 0xFFFFFFFF;
    Byte* outBuffer = nullptr;
    size_t outBufferSize = 0;
    bool anySuccess = false;

    DebugLogWrite("INFO", "7Z", "BATCH_FILE_EXTRACT_START", "requests=%zu", orderedRequests.size());

    for (const SevenZipExtractFileRequest& request : orderedRequests)
    {
        if (!request.outputPath.c_str() || !request.outputPath[0])
        {
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | reason=empty_output_path", (unsigned int)request.index);
            continue;
        }

        if (request.index >= archive.db.NumFiles)
        {
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | files=%u | reason=index_out_of_range", (unsigned int)request.index, (unsigned int)archive.db.NumFiles);
            continue;
        }

        if (SzArEx_IsDir(&archive.db, request.index))
        {
            DebugLogWrite("WARN", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | reason=index_is_folder", (unsigned int)request.index);
            continue;
        }

        UInt64 expectedSize64 = SzArEx_GetFileSize(&archive.db, request.index);

        if (request.expectedSize != 0 && expectedSize64 != request.expectedSize)
        {
            DebugLogWrite("WARN", "7Z", "BATCH_FILE_EXTRACT_SIZE_MISMATCH", "index=%u | expected_request=%llu | expected_archive=%llu", (unsigned int)request.index, (unsigned long long)request.expectedSize, (unsigned long long)expectedSize64);
        }

        UInt32 folderIndex = 0xFFFFFFFF;

        if (archive.db.FileToFolder)
        {
            folderIndex = archive.db.FileToFolder[request.index];
        }

        size_t offset = 0;
        size_t outSizeProcessed = 0;

        DebugLogWrite("INFO", "7Z", "BATCH_FILE_EXTRACT_ITEM_START", "index=%u | folder=%u | previous_block=%u | expected=%llu", (unsigned int)request.index, (unsigned int)folderIndex, (unsigned int)blockIndex, (unsigned long long)expectedSize64);

        SevenZipScopedPassword scope(password);
        SRes res = SzArEx_Extract(&archive.db, &archive.lookStream.vt, request.index, &blockIndex, &outBuffer, &outBufferSize, &offset, &outSizeProcessed, &g_Alloc, &g_AllocTemp);

        if (res != SZ_OK)
        {
            std::string itemError = MakeError("SzArEx_Extract failed during batch extract-to-file", res);
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | folder=%u | res=%d | error=%s", (unsigned int)request.index, (unsigned int)folderIndex, (int)res, itemError.c_str());

            if (error.empty())
            {
                error = itemError;
            }

            continue;
        }

        if (!outBuffer && outSizeProcessed > 0)
        {
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | processed=%zu | reason=no_output_buffer", (unsigned int)request.index, outSizeProcessed);

            if (error.empty())
            {
                error = "7z batch extraction failed: decoder returned no output buffer";
            }

            continue;
        }

        if (outSizeProcessed != static_cast<size_t>(expectedSize64))
        {
            DebugLogWrite("WARN", "7Z", "BATCH_FILE_EXTRACT_SIZE_MISMATCH", "index=%u | expected=%llu | processed=%zu", (unsigned int)request.index, (unsigned long long)expectedSize64, outSizeProcessed);
        }

        const Byte* fileData = outBuffer + offset;
        std::string writeError;

        if (!WriteBufferToFileW(request.outputPath.c_str(), fileData, outSizeProcessed, writeError))
        {
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | bytes=%zu | reason=write_output_failed | error=%s", (unsigned int)request.index, outSizeProcessed, writeError.c_str());

            if (error.empty())
            {
                error = writeError;
            }

            continue;
        }

        try
        {
            outSucceededIndexes.push_back(request.index);
        }
        catch (const std::bad_alloc&)
        {
            DebugLogWrite("ERROR", "7Z", "BATCH_FILE_EXTRACT_ITEM_FAIL", "index=%u | reason=success_vector_alloc_failed", (unsigned int)request.index);

            if (error.empty())
            {
                error = "Not enough memory to record successful 7z batch extraction";
            }

            DeleteFileW(request.outputPath.c_str());
            continue;
        }

        anySuccess = true;

        DebugLogWrite("INFO", "7Z", "BATCH_FILE_EXTRACT_ITEM_DONE", "index=%u | folder=%u | block=%u | offset=%zu | bytes=%zu", (unsigned int)request.index, (unsigned int)folderIndex, (unsigned int)blockIndex, offset, outSizeProcessed);
    }

    if (outBuffer)
    {
        ISzAlloc_Free(&g_Alloc, outBuffer);
    }

    DebugLogWrite("INFO", "7Z", "BATCH_FILE_EXTRACT_DONE", "requests=%zu | succeeded=%zu", orderedRequests.size(), outSucceededIndexes.size());

    if (!anySuccess && error.empty())
    {
        error = "No 7z batch extraction requests succeeded";
    }

    return anySuccess;
}

bool SevenZipExtractEntriesToFiles(const wchar_t* archivePath, const std::vector<SevenZipExtractFileRequest>& requests, std::vector<uint32_t>& outSucceededIndexes, std::string& error)
{
    outSucceededIndexes.clear();
    std::vector<SevenZipExtractFileRequest> pending = requests;
    return SevenZipWithPasswords(archivePath, "", [&](const std::string& password, std::string& attemptError)
    {
        std::vector<uint32_t> completed;
        SevenZipExtractEntriesToFilesCore(archivePath, pending, completed, attemptError, password);
        for (uint32_t index : completed)
        {
            if (std::find(outSucceededIndexes.begin(), outSucceededIndexes.end(), index) == outSucceededIndexes.end())
                outSucceededIndexes.push_back(index);
        }
        pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const SevenZipExtractFileRequest& request)
        {
            return std::find(completed.begin(), completed.end(), request.index) != completed.end();
        }), pending.end());
        // Partial unencrypted batches must continue trying passwords for
        // encrypted members rather than prematurely declaring the pass done.
        return pending.empty();
    }, error);
}

bool SevenZipOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error)
{
    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;

    // The same saved/runtime/password-prompt path handles encrypted archive
    // headers and encrypted packed data. Only validated passwords are retained.
    return SevenZipWithPasswords(archivePath, std::to_string(index), [&](const std::string& password, std::string& attemptError)
    {
        SevenZipScopedPassword scope(password);
        ArchiveBackendStream candidate{};
        if (!SevenZipOpenNonSolidEntryStreamImpl(archivePath, index, candidate, attemptError))
            return false;

        // A wrong AES password can initialise CBC successfully. Probe the
        // compressed plaintext before accepting and remembering a candidate.
        // All bytes are decoded, not CRC-calculated; seek resets the CBC IV.
        if (!password.empty() && candidate.size > 0)
        {
            unsigned char probe[4096];
            const size_t expected = candidate.size < sizeof(probe) ? static_cast<size_t>(candidate.size) : sizeof(probe);
            const size_t got = SevenZipReadNonSolidEntryStreamImpl(candidate, probe, expected);
            if (got != expected || !SevenZipSeekNonSolidEntryStreamImpl(candidate, 0))
            {
                attemptError = "7z AES stream password verification failed";
                SevenZipCloseNonSolidEntryStreamImpl(candidate);
                return false;
            }
        }

        // Preserve the public stream's format, read status and cancellation
        // fields; only transfer ownership of the decoder handle and counters.
        stream.impl = candidate.impl;
        stream.size = candidate.size;
        stream.position = candidate.position;
        return true;
    }, error, true);
}
size_t SevenZipReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    return SevenZipReadNonSolidEntryStreamImpl(stream, buffer, bytesToRead);
}

bool SevenZipSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    return SevenZipSeekNonSolidEntryStreamImpl(stream, absolutePosition);
}

void SevenZipCloseEntryStream(ArchiveBackendStream& stream)
{
    SevenZipCloseNonSolidEntryStreamImpl(stream);
}

bool SevenZipOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error);

size_t SevenZipReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead);

bool SevenZipSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition);

void SevenZipCloseEntryStream(ArchiveBackendStream& stream);