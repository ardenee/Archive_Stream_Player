#include "archive_7z_stream_reader.h"

#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <sstream>
#include <new>
#include <algorithm>
#include "debug_log.h"

extern "C" {
#include "7z.h"
#include "7zAlloc.h"
#include "7zCrc.h"
#include "7zFile.h"
#include "Lzma2Dec.h"
}

#define STREAM_7Z_METHOD_COPY  0x00
#define STREAM_7Z_METHOD_LZMA2 0x21

static void* StreamSevenZipAlloc(ISzAllocPtr, size_t size)
{
    return std::malloc(size);
}

static void StreamSevenZipFree(ISzAllocPtr, void* address)
{
    std::free(address);
}

static ISzAlloc g_StreamAlloc =
{
    StreamSevenZipAlloc,
    StreamSevenZipFree
};

static ISzAlloc g_StreamAllocTemp =
{
    StreamSevenZipAlloc,
    StreamSevenZipFree
};

static const char* StreamSzErrorToString(SRes res)
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

static std::string StreamMakeError(const char* message, SRes res)
{
    std::ostringstream oss;
    oss << message << ": " << StreamSzErrorToString(res) << " (" << static_cast<int>(res) << ")";
    return oss.str();
}

enum SevenZipStreamMode
{
    SEVENZIP_STREAM_MODE_NONE = 0,
    SEVENZIP_STREAM_MODE_COPY = 1,
    SEVENZIP_STREAM_MODE_LZMA2 = 2
};

static const char* SevenZipStreamModeName(SevenZipStreamMode mode)
{
    switch (mode)
    {
    case SEVENZIP_STREAM_MODE_COPY:
        return "COPY";

    case SEVENZIP_STREAM_MODE_LZMA2:
        return "LZMA2";

    case SEVENZIP_STREAM_MODE_NONE:
    default:
        return "NONE";
    }
}

static const char* SevenZipMethodName(UInt64 methodId)
{
    switch (methodId)
    {
    case 0x00:
        return "COPY";

    case 0x03:
        return "DELTA";

    case 0x21:
        return "LZMA2";

    case 0x030101:
        return "LZMA";

    case 0x03030103:
        return "BCJ_X86";

    case 0x0303011B:
        return "BCJ2";

    case 0x030401:
        return "PPMD";

    case 0x06F10701:
        return "AES256";

    default:
        return "UNKNOWN";
    }
}

// -----------------------------------------------------------------------------
// 7z split-volume stream support for non-solid streaming
//
// 7-Zip split archives commonly use:
//     album.7z.001
//     album.7z.002
//     album.7z.003
//
// The normal 7z reader already exposes those parts as one logical stream. The
// non-solid streaming reader needs the same behaviour; otherwise SzArEx_Open()
// sees only .001 and fails with SZ_ERROR_INPUT_EOF.
//
// This stream presents all numbered parts as one seekable ISeekInStream so the
// existing 7z SDK parser and simple Copy/LZMA2 streaming path can work unchanged.
// -----------------------------------------------------------------------------
struct SevenZipStreamVolumePart
{
    HANDLE handle = INVALID_HANDLE_VALUE;
    UInt64 size = 0;
    std::wstring path;
};

struct SevenZipStreamConcatInStream
{
    ISeekInStream vt{};
    std::vector<SevenZipStreamVolumePart> parts;
    UInt64 totalSize = 0;
    UInt64 position = 0;
};

static SevenZipStreamConcatInStream* SevenZipStreamConcatFromStream(const ISeekInStream* stream)
{
    return reinterpret_cast<SevenZipStreamConcatInStream*>(const_cast<ISeekInStream*>(stream));
}

static bool StreamWideEndsWithInsensitive(const wchar_t* value, const wchar_t* suffix)
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

static bool IsSevenZipSplitFirstVolumePathForStreamW(const wchar_t* archivePath)
{
    return StreamWideEndsWithInsensitive(archivePath, L".7z.001");
}

static bool BuildSevenZipStreamSplitVolumePath(const std::wstring& firstVolumePath, unsigned int volumeNumber, std::wstring& outPath)
{
    outPath.clear();

    if (firstVolumePath.length() < 3)
    {
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_PATH_FAIL", "first_volume=%ls | volume=%u | reason=path_too_short", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    if (volumeNumber == 0 || volumeNumber > 999)
    {
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_PATH_FAIL", "first_volume=%ls | volume=%u | reason=invalid_volume_number", firstVolumePath.c_str(), volumeNumber);
        return false;
    }

    wchar_t suffix[8];
    swprintf_s(suffix, L"%03u", volumeNumber);

    outPath = firstVolumePath.substr(0, firstVolumePath.length() - 3);
    outPath += suffix;

    return true;
}

static SRes SevenZipStreamConcatRead(const ISeekInStream* stream, void* buffer, size_t* size)
{
    if (!stream || !buffer || !size)
    {
        return SZ_ERROR_PARAM;
    }

    SevenZipStreamConcatInStream* concat = SevenZipStreamConcatFromStream(stream);
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
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_READ_FAIL", "position=%llu | total_size=%llu | reason=part_not_found", (unsigned long long)concat->position, (unsigned long long)concat->totalSize);
            return SZ_ERROR_READ;
        }

        SevenZipStreamVolumePart& part = concat->parts[partIndex];

        if (part.handle == INVALID_HANDLE_VALUE)
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_READ_FAIL", "part=%zu | path=%ls | reason=invalid_handle", partIndex, part.path.c_str());
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
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_READ_FAIL", "part=%zu | path=%ls | offset=%llu | reason=seek_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, GetLastError());
            return SZ_ERROR_READ;
        }

        DWORD bytesRead = 0;

        if (!ReadFile(part.handle, out + totalRead, static_cast<DWORD>(want), &bytesRead, NULL))
        {
            *size = totalRead;
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_READ_FAIL", "part=%zu | path=%ls | offset=%llu | requested=%zu | reason=read_failed | winerr=%lu", partIndex, part.path.c_str(), (unsigned long long)partOffset64, want, GetLastError());
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

static SRes SevenZipStreamConcatSeek(const ISeekInStream* stream, Int64* pos, ESzSeek origin)
{
    if (!stream || !pos)
    {
        return SZ_ERROR_PARAM;
    }

    SevenZipStreamConcatInStream* concat = SevenZipStreamConcatFromStream(stream);
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
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_SEEK_FAIL", "origin=%d | reason=unsupported_origin", (int)origin);
        return SZ_ERROR_PARAM;
    }

    if (newPos < 0)
    {
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_SEEK_FAIL", "target=%lld | reason=negative_position", (long long)newPos);
        return SZ_ERROR_PARAM;
    }

    concat->position = static_cast<UInt64>(newPos);
    *pos = newPos;

    return SZ_OK;
}

static void SevenZipStreamConcatInit(SevenZipStreamConcatInStream* stream)
{
    if (!stream)
    {
        return;
    }

    stream->vt.Read = SevenZipStreamConcatRead;
    stream->vt.Seek = SevenZipStreamConcatSeek;
    stream->totalSize = 0;
    stream->position = 0;
    stream->parts.clear();
}

static void SevenZipStreamConcatClose(SevenZipStreamConcatInStream* stream)
{
    if (!stream)
    {
        return;
    }

    for (SevenZipStreamVolumePart& part : stream->parts)
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

static bool SevenZipStreamConcatOpen(SevenZipStreamConcatInStream* stream, const wchar_t* firstVolumePath, std::string& error)
{
    if (!stream || !firstVolumePath || !*firstVolumePath)
    {
        error = "7z stream split open failed: invalid input";
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "reason=invalid_input");
        return false;
    }

    SevenZipStreamConcatClose(stream);
    SevenZipStreamConcatInit(stream);

    std::wstring firstPath(firstVolumePath);
    bool openedAny = false;

    for (unsigned int volumeNumber = 1; volumeNumber <= 999; ++volumeNumber)
    {
        std::wstring partPath;

        if (!BuildSevenZipStreamSplitVolumePath(firstPath, volumeNumber, partPath))
        {
            error = "7z stream split open failed: failed to build volume path";
            SevenZipStreamConcatClose(stream);
            return false;
        }

        DWORD attrs = GetFileAttributesW(partPath.c_str());

        if (attrs == INVALID_FILE_ATTRIBUTES)
        {
            if (!openedAny)
            {
                error = "7z stream split open failed: first volume does not exist";
                DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "path=%ls | reason=first_volume_missing | winerr=%lu", partPath.c_str(), GetLastError());
                SevenZipStreamConcatClose(stream);
                return false;
            }

            DebugLogWrite("INFO", "7Z", "STREAM_SPLIT_OPEN_DONE", "volumes=%zu | total_size=%llu | missing_next=%ls", stream->parts.size(), (unsigned long long)stream->totalSize, partPath.c_str());
            break;
        }

        if (attrs & FILE_ATTRIBUTE_DIRECTORY)
        {
            error = "7z stream split open failed: volume path is a directory";
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "path=%ls | reason=volume_is_directory", partPath.c_str());
            SevenZipStreamConcatClose(stream);
            return false;
        }

        HANDLE handle = CreateFileW(partPath.c_str(), GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);

        if (handle == INVALID_HANDLE_VALUE)
        {
            error = "7z stream split open failed: failed to open volume";
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "path=%ls | reason=open_volume_failed | winerr=%lu", partPath.c_str(), GetLastError());
            SevenZipStreamConcatClose(stream);
            return false;
        }

        LARGE_INTEGER fileSize;

        if (!GetFileSizeEx(handle, &fileSize) || fileSize.QuadPart < 0)
        {
            error = "7z stream split open failed: failed to read volume size";
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "path=%ls | reason=get_size_failed | winerr=%lu", partPath.c_str(), GetLastError());
            CloseHandle(handle);
            SevenZipStreamConcatClose(stream);
            return false;
        }

        SevenZipStreamVolumePart part;
        part.handle = handle;
        part.size = static_cast<UInt64>(fileSize.QuadPart);
        part.path = partPath;

        try
        {
            stream->parts.push_back(part);
        }
        catch (const std::bad_alloc&)
        {
            error = "7z stream split open failed: not enough memory for volume list";
            DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "path=%ls | reason=volume_list_alloc_failed", partPath.c_str());
            CloseHandle(handle);
            SevenZipStreamConcatClose(stream);
            return false;
        }

        stream->totalSize += part.size;
        openedAny = true;

        //DebugLogWrite("INFO", "7Z", "STREAM_SPLIT_OPEN_PART", "volume=%u | path=%ls | size=%llu | total_size=%llu", volumeNumber, partPath.c_str(), (unsigned long long)part.size, (unsigned long long)stream->totalSize);
    }

    if (stream->parts.empty() || stream->totalSize == 0)
    {
        error = "7z stream split open failed: no usable volume data";
        DebugLogWrite("ERROR", "7Z", "STREAM_SPLIT_OPEN_FAIL", "reason=no_volume_data");
        SevenZipStreamConcatClose(stream);
        return false;
    }

    return true;
}

struct SevenZipNonSolidStreamState
{
    CFileInStream fileStream{};
    SevenZipStreamConcatInStream concatStream{};
    CLookToRead2 lookStream{};
    CSzArEx db{};
    bool fileOpen = false;
    bool concatOpen = false;
    bool dbOpen = false;

    std::wstring archivePath;
    uint32_t index = 0;

    UInt32 folderIndex = 0xFFFFFFFF;
    UInt64 packStart = 0;
    UInt64 packSize = 0;

    uint64_t size = 0;
    uint64_t position = 0;

    SevenZipStreamMode mode = SEVENZIP_STREAM_MODE_NONE;

    // Packed input progress. For Copy this mirrors output position.
    // For LZMA2 this tracks compressed bytes consumed.
    UInt64 packBytesRead = 0;

    // LZMA2 streaming state.
    CLzma2Dec lzma2{};
    bool lzma2Constructed = false;
    bool lzma2Allocated = false;
    bool lzma2Finished = false;
    size_t lzma2OutputReadPos = 0;
};

// -----------------------------------------------------------------------------
// OpenArchiveForStream
//
// Opens a normal .7z archive or a first-volume split .7z.001 archive for the
// non-solid stream reader.
//
// Normal .7z files use the 7-Zip SDK CFileInStream. Split .7z.001 files use the
// local SevenZipStreamConcatInStream wrapper so SzArEx_Open() sees all numbered
// parts as one logical seekable stream. Without this, stream preflight on split
// 7z archives fails with SZ_ERROR_INPUT_EOF after reading only .001.
// -----------------------------------------------------------------------------
static bool OpenArchiveForStream(SevenZipNonSolidStreamState* state, const wchar_t* archivePath, std::string& error)
{
    if (!state || !archivePath || !*archivePath)
    {
        error = "7z stream open failed: archive path is empty";
        DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "reason=empty_archive_path");
        return false;
    }

    FileInStream_CreateVTable(&state->fileStream);
    SevenZipStreamConcatInit(&state->concatStream);
    LookToRead2_CreateVTable(&state->lookStream, False);
    SzArEx_Init(&state->db);

    bool useSplitStream = IsSevenZipSplitFirstVolumePathForStreamW(archivePath);

    if (useSplitStream)
    {
        if (!SevenZipStreamConcatOpen(&state->concatStream, archivePath, error))
        {
            DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "path=%ls | reason=split_stream_open_failed | error=%s", archivePath, error.c_str());
            return false;
        }

        state->concatOpen = true;
        state->lookStream.realStream = &state->concatStream.vt;

        DebugLogWrite("INFO", "7Z", "STREAM_OPEN_SPLIT_STREAM", "path=%ls | volumes=%zu | total_size=%llu", archivePath, state->concatStream.parts.size(), (unsigned long long)state->concatStream.totalSize);
    }
    else
    {
        if (InFile_OpenW(&state->fileStream.file, archivePath) != 0)
        {
            error = "7z stream open failed: could not open archive file";
            DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "path=%ls | reason=open_archive_file_failed", archivePath);
            return false;
        }

        state->fileOpen = true;
        state->lookStream.realStream = &state->fileStream.vt;
    }

    state->lookStream.bufSize = 1 << 18; // 256 KiB
    state->lookStream.buf = static_cast<Byte*>(ISzAlloc_Alloc(&g_StreamAlloc, state->lookStream.bufSize));

    if (!state->lookStream.buf)
    {
        error = "7z stream open failed: could not allocate input buffer";
        DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "path=%ls | bytes=%zu | reason=input_buffer_alloc_failed", archivePath, state->lookStream.bufSize);
        return false;
    }

    LookToRead2_INIT(&state->lookStream);
    CrcGenerateTable();

    SRes res = SzArEx_Open(&state->db, &state->lookStream.vt, &g_StreamAlloc, &g_StreamAllocTemp);

    if (res != SZ_OK)
    {
        error = StreamMakeError("7z stream open failed: SzArEx_Open failed", res);
        DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "path=%ls | split=%d | res=%d | error=%s", archivePath, useSplitStream ? 1 : 0, (int)res, error.c_str());
        return false;
    }

    state->dbOpen = true;

    DebugLogWrite("INFO", "7Z", "STREAM_OPEN_OK", "path=%ls | split=%d | files=%u | folders=%u", archivePath, useSplitStream ? 1 : 0, (unsigned int)state->db.NumFiles, (unsigned int)state->db.db.NumFolders);

    return true;
}

static void FreeLzma2State(SevenZipNonSolidStreamState* state)
{
    if (!state)
        return;

    if (state->lzma2Allocated)
    {
        Lzma2Dec_Free(&state->lzma2, &g_StreamAlloc);
        state->lzma2Allocated = false;
    }

    state->lzma2Constructed = false;
    state->lzma2Finished = false;
    state->lzma2OutputReadPos = 0;
}

static void CloseArchiveForStream(SevenZipNonSolidStreamState* state)
{
    if (!state)
        return;

    FreeLzma2State(state);

    if (state->dbOpen)
    {
        SzArEx_Free(&state->db, &g_StreamAlloc);
        state->dbOpen = false;
    }

    if (state->lookStream.buf)
    {
        ISzAlloc_Free(&g_StreamAlloc, state->lookStream.buf);
        state->lookStream.buf = nullptr;
        state->lookStream.bufSize = 0;
    }

    if (state->concatOpen)
    {
        SevenZipStreamConcatClose(&state->concatStream);
        state->concatOpen = false;
    }

    if (state->fileOpen)
    {
        File_Close(&state->fileStream.file);
        state->fileOpen = false;
    }
}

static uint32_t CountRealFilesInFolder(const CSzArEx* db, UInt32 folderIndex)
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

static bool ReadFolderForStream(const CSzArEx* db, UInt32 folderIndex, CSzFolder* outFolder, std::string& error)
{
    if (!db || !outFolder)
    {
        error = "7z stream open failed: invalid folder metadata";
        DebugLogWrite("WARN", "7Z", "STREAM_FOLDER_FAIL", "folder=%u | reason=invalid_metadata", (unsigned int)folderIndex);
        return false;
    }

    if (folderIndex >= db->db.NumFolders)
    {
        error = "7z stream open failed: folder index out of range";
        DebugLogWrite("WARN", "7Z", "STREAM_FOLDER_FAIL", "folder=%u | folders=%u | reason=folder_index_out_of_range", (unsigned int)folderIndex, (unsigned int)db->db.NumFolders);
        return false;
    }

    CSzData sd;
    const Byte* data = db->db.CodersData + db->db.FoCodersOffsets[folderIndex];

    sd.Data = data;
    sd.Size = db->db.FoCodersOffsets[(size_t)folderIndex + 1] - db->db.FoCodersOffsets[folderIndex];

    SRes res = SzGetNextFolderItem(outFolder, &sd);

    if (res != SZ_OK)
    {
        error = StreamMakeError("7z stream open failed: SzGetNextFolderItem failed", res);
        DebugLogWrite("WARN", "7Z", "STREAM_FOLDER_FAIL", "folder=%u | res=%d | error=%s", (unsigned int)folderIndex, (int)res, error.c_str());
        return false;
    }

    if (sd.Size != 0)
    {
        error = "7z stream open failed: unexpected remaining coder metadata";
        DebugLogWrite("WARN", "7Z", "STREAM_FOLDER_FAIL", "folder=%u | remaining=%zu | reason=unexpected_remaining_coder_metadata", (unsigned int)folderIndex, sd.Size);
        return false;
    }

    return true;
}

static bool ValidateSimpleFolderForStream(const CSzArEx* db, UInt32 folderIndex, CSzFolder* folder, const Byte* coderPropsBase, size_t coderPropsSize, SevenZipStreamMode* outMode, Byte* outLzma2Prop, std::string& error)
{
    if (outMode)
        *outMode = SEVENZIP_STREAM_MODE_NONE;

    if (outLzma2Prop)
        *outLzma2Prop = 0;

    if (!db || !folder || !outMode)
    {
        error = "7z stream open failed: invalid folder";
        DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | reason=invalid_folder", (unsigned int)folderIndex);
        return false;
    }

    if (folder->NumCoders != 1)
    {
        error = "7z stream open failed: only single-coder folders are supported in this stage";

        DebugLogWrite("INFO", "7Z", "STREAM_LAYOUT", "folder=%u | coders=%u | bonds=%u | pack_streams=%u | reason=multi_coder_layout_seen", (unsigned int)folderIndex, (unsigned int)folder->NumCoders, (unsigned int)folder->NumBonds, (unsigned int)folder->NumPackStreams);

        for (UInt32 i = 0; i < folder->NumCoders; ++i)
        {
            const CSzCoderInfo* c = &folder->Coders[i];

            DebugLogWrite("INFO", "7Z", "STREAM_LAYOUT_CODER", "folder=%u | coder=%u | method=0x%llx | method_name=%s | streams=%u | props_offset=%zu | props_size=%zu", (unsigned int)folderIndex, (unsigned int)i, (unsigned long long)c->MethodID, SevenZipMethodName(c->MethodID), (unsigned int)c->NumStreams, (size_t)c->PropsOffset, (size_t)c->PropsSize);
        }

        for (UInt32 i = 0; i < folder->NumBonds; ++i)
        {
            DebugLogWrite("INFO", "7Z", "STREAM_LAYOUT_BOND", "folder=%u | bond=%u | in_index=%u | out_index=%u", (unsigned int)folderIndex, (unsigned int)i, (unsigned int)folder->Bonds[i].InIndex, (unsigned int)folder->Bonds[i].OutIndex);
        }

        for (UInt32 i = 0; i < folder->NumPackStreams; ++i)
        {
            DebugLogWrite("INFO", "7Z", "STREAM_LAYOUT_PACK", "folder=%u | pack_stream=%u | stream_index=%u", (unsigned int)folderIndex, (unsigned int)i, (unsigned int)folder->PackStreams[i]);
        }

        DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | coders=%u | bonds=%u | pack_streams=%u | reason=unsupported_coder_count", (unsigned int)folderIndex, (unsigned int)folder->NumCoders, (unsigned int)folder->NumBonds, (unsigned int)folder->NumPackStreams);

        return false;
    }

    if (folder->NumPackStreams != 1 || folder->PackStreams[0] != 0)
    {
        error = "7z stream open failed: unsupported pack stream layout";
        DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | pack_streams=%u | first_pack=%u | reason=unsupported_pack_stream_layout", (unsigned int)folderIndex, (unsigned int)folder->NumPackStreams, folder->NumPackStreams > 0 ? (unsigned int)folder->PackStreams[0] : 0U);
        return false;
    }

    if (folder->NumBonds != 0)
    {
        error = "7z stream open failed: bonded coder pipelines are not supported in this stage";
        DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | bonds=%u | reason=bonded_pipeline_not_supported", (unsigned int)folderIndex, (unsigned int)folder->NumBonds);
        return false;
    }

    const CSzCoderInfo* coder = &folder->Coders[0];

    if (coder->NumStreams != 1)
    {
        error = "7z stream open failed: unsupported coder stream count";
        DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | streams=%u | reason=unsupported_coder_stream_count", (unsigned int)folderIndex, (unsigned int)coder->NumStreams);
        return false;
    }

    if (coder->MethodID == STREAM_7Z_METHOD_COPY)
    {
        if (coder->PropsSize != 0)
        {
            error = "7z stream open failed: Copy method unexpectedly has properties";
            DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | method=COPY | props=%zu | reason=unexpected_copy_properties", (unsigned int)folderIndex, (size_t)coder->PropsSize);
            return false;
        }

        *outMode = SEVENZIP_STREAM_MODE_COPY;
        return true;
    }

    if (coder->MethodID == STREAM_7Z_METHOD_LZMA2)
    {
        if (coder->PropsSize != 1)
        {
            error = "7z stream open failed: LZMA2 method must have one property byte";
            DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | method=LZMA2 | props=%zu | reason=invalid_lzma2_props_size", (unsigned int)folderIndex, (size_t)coder->PropsSize);
            return false;
        }

        if (!coderPropsBase)
        {
            error = "7z stream open failed: missing coder properties buffer";
            DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | method=LZMA2 | reason=missing_coder_props_buffer", (unsigned int)folderIndex);
            return false;
        }

        if (coder->PropsOffset + coder->PropsSize > coderPropsSize)
        {
            error = "7z stream open failed: LZMA2 property offset is out of range";
            DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | method=LZMA2 | props_offset=%zu | props_size=%zu | buffer_size=%zu | reason=props_offset_out_of_range", (unsigned int)folderIndex, (size_t)coder->PropsOffset, (size_t)coder->PropsSize, coderPropsSize);
            return false;
        }

        *outMode = SEVENZIP_STREAM_MODE_LZMA2;

        if (outLzma2Prop)
            *outLzma2Prop = coderPropsBase[coder->PropsOffset];

        return true;
    }

    error = "7z stream open failed: unsupported method for streaming";
    DebugLogWrite("WARN", "7Z", "STREAM_VALIDATE_FAIL", "folder=%u | method=%llu | reason=unsupported_method", (unsigned int)folderIndex, (unsigned long long)coder->MethodID);
    return false;
}

static bool SeekToPackedStart(SevenZipNonSolidStreamState* state, std::string& error)
{
    if (!state)
    {
        error = "7z stream seek failed: invalid state";
        DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "reason=invalid_state");
        return false;
    }

    SRes seekRes = LookInStream_SeekTo(&state->lookStream.vt, state->packStart);

    if (seekRes != SZ_OK)
    {
        error = StreamMakeError("7z stream seek failed: seek to packed stream failed", seekRes);
        DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | pack_start=%llu | res=%d | error=%s", (unsigned int)state->index, (unsigned long long)state->packStart, (int)seekRes, error.c_str());
        return false;
    }

    state->packBytesRead = 0;
    return true;
}

static bool InitLzma2Decoder(SevenZipNonSolidStreamState* state, Byte prop, std::string& error)
{
    if (!state)
    {
        error = "7z LZMA2 init failed: invalid state";
        DebugLogWrite("ERROR", "7Z", "LZMA2_INIT_FAIL", "reason=invalid_state");
        return false;
    }

    FreeLzma2State(state);

    Lzma2Dec_Construct(&state->lzma2);
    state->lzma2Constructed = true;

    SRes res = Lzma2Dec_Allocate(&state->lzma2, prop, &g_StreamAlloc);

    if (res != SZ_OK)
    {
        error = StreamMakeError("7z LZMA2 init failed: Lzma2Dec_Allocate failed", res);
        DebugLogWrite("ERROR", "7Z", "LZMA2_INIT_FAIL", "index=%u | prop=%u | res=%d | error=%s", (unsigned int)state->index, (unsigned int)prop, (int)res, error.c_str());
        FreeLzma2State(state);
        return false;
    }

    state->lzma2Allocated = true;
    Lzma2Dec_Init(&state->lzma2);

    state->lzma2Finished = false;
    state->lzma2OutputReadPos = 0;

    return true;
}

static size_t ReadCopyBytes(SevenZipNonSolidStreamState* state, void* buffer, size_t bytesToRead)
{
    if (!state || !buffer || bytesToRead == 0)
        return 0;

    if (state->position >= state->size)
        return 0;

    uint64_t remaining64 = state->size - state->position;

    if (bytesToRead > remaining64)
        bytesToRead = static_cast<size_t>(remaining64);

    size_t totalRead = 0;
    unsigned char* out = static_cast<unsigned char*>(buffer);

    while (totalRead < bytesToRead)
    {
        const void* inBuf = nullptr;
        size_t want = bytesToRead - totalRead;

        if (want > (1 << 18))
            want = 1 << 18;

        SRes res = ILookInStream_Look(&state->lookStream.vt, &inBuf, &want);

        if (res != SZ_OK || want == 0 || !inBuf)
        {
            if (state->position < state->size)
                DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | size=%llu | requested=%zu | wanted=%zu | res=%d | mode=COPY | reason=look_failed_or_empty", (unsigned int)state->index, (unsigned long long)state->position, (unsigned long long)state->size, bytesToRead, want, (int)res);
            break;
        }

        std::memcpy(out + totalRead, inBuf, want);

        res = ILookInStream_Skip(&state->lookStream.vt, want);

        if (res != SZ_OK)
        {
            DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | size=%llu | skip=%zu | res=%d | mode=COPY | reason=skip_failed", (unsigned int)state->index, (unsigned long long)state->position, (unsigned long long)state->size, want, (int)res);
            break;
        }

        totalRead += want;
        state->position += want;
        state->packBytesRead += want;
    }

    return totalRead;
}

static size_t CopyPendingLzma2Output(SevenZipNonSolidStreamState* state, unsigned char* out, size_t outCapacity)
{
    if (!state || !out || outCapacity == 0)
        return 0;

    size_t dicPos = state->lzma2.decoder.dicPos;

    if (state->lzma2OutputReadPos >= dicPos)
        return 0;

    size_t available = dicPos - state->lzma2OutputReadPos;
    uint64_t remainingOutput64 = state->size - state->position;

    if (available > remainingOutput64)
        available = static_cast<size_t>(remainingOutput64);

    size_t toCopy = available < outCapacity ? available : outCapacity;

    std::memcpy(out, state->lzma2.decoder.dic + state->lzma2OutputReadPos, toCopy);

    state->lzma2OutputReadPos += toCopy;
    state->position += toCopy;

    return toCopy;
}

static bool ResetLzma2OutputWindowIfConsumed(SevenZipNonSolidStreamState* state)
{
    if (!state)
        return false;

    /*
        The SDK decoder writes to decoder.dic up to decoder.dicBufSize.
        Once all produced bytes have been consumed and the dictionary is full,
        restart the write cursor at 0. The decoder state keeps the history
        needed for back-references.
    */
    if (state->lzma2.decoder.dicPos == state->lzma2.decoder.dicBufSize &&
        state->lzma2OutputReadPos == state->lzma2.decoder.dicPos)
    {
        state->lzma2.decoder.dicPos = 0;
        state->lzma2OutputReadPos = 0;
        return true;
    }

    return false;
}

static size_t ReadLzma2Bytes(SevenZipNonSolidStreamState* state, void* buffer, size_t bytesToRead)
{
    if (!state || !buffer || bytesToRead == 0)
        return 0;

    if (state->position >= state->size)
        return 0;

    unsigned char* out = static_cast<unsigned char*>(buffer);
    size_t totalOut = 0;

    while (totalOut < bytesToRead && state->position < state->size)
    {
        size_t copied = CopyPendingLzma2Output(state, out + totalOut, bytesToRead - totalOut);

        if (copied > 0)
        {
            totalOut += copied;
            continue;
        }

        if (state->lzma2Finished)
            break;

        ResetLzma2OutputWindowIfConsumed(state);

        if (state->lzma2.decoder.dicPos >= state->lzma2.decoder.dicBufSize)
            break;

        if (state->packBytesRead >= state->packSize)
            break;

        const void* inBuf = nullptr;

        UInt64 packedRemaining64 = state->packSize - state->packBytesRead;
        size_t srcLen = packedRemaining64 > (UInt64)(1 << 18) ? (1 << 18) : static_cast<size_t>(packedRemaining64);

        SRes lookRes = ILookInStream_Look(&state->lookStream.vt, &inBuf, &srcLen);

        if (lookRes != SZ_OK || srcLen == 0 || !inBuf)
        {
            DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | size=%llu | pack_read=%llu | pack_size=%llu | look_res=%d | src_len=%zu | mode=LZMA2 | reason=look_failed_or_empty", (unsigned int)state->index, (unsigned long long)state->position, (unsigned long long)state->size, (unsigned long long)state->packBytesRead, (unsigned long long)state->packSize, (int)lookRes, srcLen);
            break;
        }

        SizeT srcProcessed = srcLen;
        ELzmaStatus status = LZMA_STATUS_NOT_SPECIFIED;

        size_t oldDicPos = state->lzma2.decoder.dicPos;

        SRes decRes = Lzma2Dec_DecodeToDic(&state->lzma2, state->lzma2.decoder.dicBufSize, static_cast<const Byte*>(inBuf), &srcProcessed, LZMA_FINISH_ANY, &status);

        if (srcProcessed > 0)
        {
            SRes skipRes = ILookInStream_Skip(&state->lookStream.vt, srcProcessed);

            if (skipRes != SZ_OK)
            {
                DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | src_processed=%zu | skip_res=%d | mode=LZMA2 | reason=skip_failed", (unsigned int)state->index, (unsigned long long)state->position, (size_t)srcProcessed, (int)skipRes);
                break;
            }

            state->packBytesRead += srcProcessed;
        }

        if (decRes != SZ_OK)
        {
            DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | dec_res=%d | status=%d | src_processed=%zu | mode=LZMA2 | reason=decode_failed", (unsigned int)state->index, (unsigned long long)state->position, (int)decRes, (int)status, (size_t)srcProcessed);
            break;
        }

        if (status == LZMA_STATUS_FINISHED_WITH_MARK)
            state->lzma2Finished = true;

        /*
            If no input was consumed and no output was produced, avoid a
            tight loop. Cache fallback remains available at higher level
            on future opens, but during read this simply ends the stream.
        */
        if (srcProcessed == 0 && state->lzma2.decoder.dicPos == oldDicPos)
        {
            DebugLogWrite("WARN", "7Z", "STREAM_READ_FAIL", "index=%u | position=%llu | mode=LZMA2 | reason=no_input_consumed_and_no_output", (unsigned int)state->index, (unsigned long long)state->position);
            break;
        }
    }

    return totalOut;
}

static bool ResetStreamToBeginning(SevenZipNonSolidStreamState* state, std::string& error)
{
    if (!state)
    {
        error = "7z stream reset failed: invalid state";
        DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "reason=invalid_state");
        return false;
    }

    if (!SeekToPackedStart(state, error))
    {
        DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "index=%u | error=%s | reason=seek_to_packed_start_failed", (unsigned int)state->index, error.c_str());
        return false;
    }

    state->position = 0;
    state->packBytesRead = 0;

    if (state->mode == SEVENZIP_STREAM_MODE_LZMA2)
    {
        Byte prop = 0;

        /*
            The prop is already known at open time, but not stored separately.
            Re-read the folder metadata and get it again for a clean reset.
        */
        CSzFolder folder;

        if (!ReadFolderForStream(&state->db, state->folderIndex, &folder, error))
        {
            DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "index=%u | folder=%u | error=%s | reason=read_folder_failed", (unsigned int)state->index, (unsigned int)state->folderIndex, error.c_str());
            return false;
        }

        SevenZipStreamMode mode = SEVENZIP_STREAM_MODE_NONE;
        const Byte* coderPropsBase = state->db.db.CodersData + state->db.db.FoCodersOffsets[state->folderIndex];

        size_t coderPropsSize = state->db.db.FoCodersOffsets[(size_t)state->folderIndex + 1] - state->db.db.FoCodersOffsets[state->folderIndex];

        if (!ValidateSimpleFolderForStream(&state->db, state->folderIndex, &folder, coderPropsBase, coderPropsSize, &mode, &prop, error))
        {
            DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "index=%u | folder=%u | error=%s | reason=validate_folder_failed", (unsigned int)state->index, (unsigned int)state->folderIndex, error.c_str());
            return false;
        }

        if (mode != SEVENZIP_STREAM_MODE_LZMA2)
        {
            error = "7z stream reset failed: entry is no longer LZMA2";
            DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "index=%u | folder=%u | mode=%s | reason=mode_changed", (unsigned int)state->index, (unsigned int)state->folderIndex, SevenZipStreamModeName(mode));
            return false;
        }

        if (!InitLzma2Decoder(state, prop, error))
        {
            DebugLogWrite("WARN", "7Z", "STREAM_RESET_FAIL", "index=%u | folder=%u | error=%s | reason=lzma2_reinit_failed", (unsigned int)state->index, (unsigned int)state->folderIndex, error.c_str());
            return false;
        }
    }

    return true;
}

bool SevenZipOpenNonSolidEntryStreamImpl(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error)
{
    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
    error.clear();

    SevenZipNonSolidStreamState* state = new (std::nothrow) SevenZipNonSolidStreamState();

    if (!state)
    {
        error = "7z stream open failed: not enough memory";
        DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "index=%u | reason=state_alloc_failed", (unsigned int)index);
        return false;
    }

    state->archivePath = archivePath ? archivePath : L"";
    state->index = index;

    if (!OpenArchiveForStream(state, archivePath, error))
    {
        DebugLogWrite("ERROR", "7Z", "STREAM_OPEN_FAIL", "index=%u | reason=open_archive_failed | error=%s", (unsigned int)index, error.c_str());
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    if (index >= state->db.NumFiles)
    {
        error = "7z stream open failed: index out of range";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | files=%u | reason=index_out_of_range", (unsigned int)index, (unsigned int)state->db.NumFiles);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    if (SzArEx_IsDir(&state->db, index))
    {
        error = "7z stream open failed: selected index is a folder";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | reason=index_is_folder", (unsigned int)index);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    if (!state->db.FileToFolder)
    {
        error = "7z stream open failed: archive has no file-to-folder map";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | reason=no_file_to_folder_map", (unsigned int)index);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    state->folderIndex = state->db.FileToFolder[index];

    if (state->folderIndex == 0xFFFFFFFF)
    {
        error = "7z stream open failed: entry has no folder";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | reason=no_folder", (unsigned int)index);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    uint32_t filesInFolder = CountRealFilesInFolder(&state->db, state->folderIndex);

    if (filesInFolder > 1)
    {
        error = "7z stream open failed: solid 7z entries are not streamable";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | folder=%u | files_in_folder=%u | reason=solid_not_streamable", (unsigned int)index, (unsigned int)state->folderIndex, (unsigned int)filesInFolder);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    CSzFolder folder;

    if (!ReadFolderForStream(&state->db, state->folderIndex, &folder, error))
    {
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | folder=%u | reason=read_folder_failed | error=%s", (unsigned int)index, (unsigned int)state->folderIndex, error.c_str());
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    SevenZipStreamMode mode = SEVENZIP_STREAM_MODE_NONE;
    Byte lzma2Prop = 0;

    const Byte* coderPropsBase = state->db.db.CodersData + state->db.db.FoCodersOffsets[state->folderIndex];

    size_t coderPropsSize = state->db.db.FoCodersOffsets[(size_t)state->folderIndex + 1] - state->db.db.FoCodersOffsets[state->folderIndex];

    if (!ValidateSimpleFolderForStream(&state->db, state->folderIndex, &folder, coderPropsBase, coderPropsSize, &mode, &lzma2Prop, error))
    {
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | folder=%u | reason=validate_folder_failed | error=%s", (unsigned int)index, (unsigned int)state->folderIndex, error.c_str());
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    UInt64 unpackSize = SzArEx_GetFileSize(&state->db, index);
    UInt64 folderUnpackSize = SzAr_GetFolderUnpackSize(&state->db.db, state->folderIndex);

    if (unpackSize != folderUnpackSize)
    {
        error = "7z stream open failed: folder size does not match entry size";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | folder=%u | entry_size=%llu | folder_size=%llu | reason=folder_size_mismatch", (unsigned int)index, (unsigned int)state->folderIndex, (unsigned long long)unpackSize, (unsigned long long)folderUnpackSize);
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    UInt32 packStartIndex = state->db.db.FoStartPackStreamIndex[state->folderIndex];

    state->packStart = state->db.dataPos + state->db.db.PackPositions[packStartIndex];
    state->packSize = state->db.db.PackPositions[(size_t)packStartIndex + 1] - state->db.db.PackPositions[packStartIndex];

    state->mode = mode;
    state->size = static_cast<uint64_t>(unpackSize);
    state->position = 0;
    state->packBytesRead = 0;

    if (!SeekToPackedStart(state, error))
    {
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | folder=%u | reason=seek_to_packed_start_failed | error=%s", (unsigned int)index, (unsigned int)state->folderIndex, error.c_str());
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    if (state->mode == SEVENZIP_STREAM_MODE_COPY)
    {
        if (state->packSize != unpackSize)
        {
            error = "7z stream open failed: Copy pack size does not match entry size";
            DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | mode=COPY | pack_size=%llu | unpack_size=%llu | reason=copy_pack_size_mismatch", (unsigned int)index, (unsigned long long)state->packSize, (unsigned long long)unpackSize);
            CloseArchiveForStream(state);
            delete state;
            return false;
        }
    }
    else if (state->mode == SEVENZIP_STREAM_MODE_LZMA2)
    {
        if (!InitLzma2Decoder(state, lzma2Prop, error))
        {
            DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | mode=LZMA2 | reason=lzma2_init_failed | error=%s", (unsigned int)index, error.c_str());
            CloseArchiveForStream(state);
            delete state;
            return false;
        }
    }
    else
    {
        error = "7z stream open failed: unsupported stream mode";
        DebugLogWrite("WARN", "7Z", "STREAM_OPEN_FAIL", "index=%u | mode=%s | reason=unsupported_stream_mode", (unsigned int)index, SevenZipStreamModeName(state->mode));
        CloseArchiveForStream(state);
        delete state;
        return false;
    }

    stream.impl = state;
    stream.size = state->size;
    stream.position = 0;

    return true;
}

size_t SevenZipReadNonSolidEntryStreamImpl(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    if (!stream.impl || !buffer || bytesToRead == 0)
        return 0;

    SevenZipNonSolidStreamState* state = static_cast<SevenZipNonSolidStreamState*>(stream.impl);

    size_t bytesRead = 0;

    if (state->mode == SEVENZIP_STREAM_MODE_COPY)
        bytesRead = ReadCopyBytes(state, buffer, bytesToRead);
    else if (state->mode == SEVENZIP_STREAM_MODE_LZMA2)
        bytesRead = ReadLzma2Bytes(state, buffer, bytesToRead);

    stream.position = state->position;

    if (bytesRead == 0 && bytesToRead > 0 && state->position < state->size)
        DebugLogWrite("WARN", "7Z", "STREAM_READ_ZERO", "index=%u | position=%llu | size=%llu | requested=%zu | mode=%s", (unsigned int)state->index, (unsigned long long)state->position, (unsigned long long)state->size, bytesToRead, SevenZipStreamModeName(state->mode));

    return bytesRead;
}

bool SevenZipSeekNonSolidEntryStreamImpl(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    if (!stream.impl)
    {
        DebugLogWrite("ERROR", "7Z", "STREAM_SEEK_FAIL", "target=%llu | reason=no_stream_impl", (unsigned long long)absolutePosition);
        return false;
    }

    SevenZipNonSolidStreamState* state = static_cast<SevenZipNonSolidStreamState*>(stream.impl);

    if (absolutePosition > state->size)
    {
        DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | target=%llu | size=%llu | mode=%s | reason=beyond_end", (unsigned int)state->index, (unsigned long long)absolutePosition, (unsigned long long)state->size, SevenZipStreamModeName(state->mode));
        return false;
    }

    if (state->mode == SEVENZIP_STREAM_MODE_COPY)
    {
        SRes res = LookInStream_SeekTo(&state->lookStream.vt, state->packStart + absolutePosition);

        if (res != SZ_OK)
        {
            DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | target=%llu | mode=COPY | reason=seek_to_pack_failed | code=%d", (unsigned int)state->index, (unsigned long long)absolutePosition, (int)res);
            return false;
        }

        state->position = absolutePosition;
        state->packBytesRead = absolutePosition;
        stream.position = absolutePosition;
        return true;
    }

    if (state->mode == SEVENZIP_STREAM_MODE_LZMA2)
    {
        if (absolutePosition == state->position)
            return true;

        std::string error;

        if (!ResetStreamToBeginning(state, error))
        {
            DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | target=%llu | mode=LZMA2 | reason=reset_failed | error=%s", (unsigned int)state->index, (unsigned long long)absolutePosition, error.c_str());
            return false;
        }

        unsigned char temp[64 * 1024];
        uint64_t remaining = absolutePosition;

        while (remaining > 0)
        {
            size_t want = remaining > sizeof(temp) ? sizeof(temp) : static_cast<size_t>(remaining);
            size_t got = ReadLzma2Bytes(state, temp, want);

            if (got == 0)
            {
                DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | target=%llu | current=%llu | mode=LZMA2 | reason=discard_read_zero", (unsigned int)state->index, (unsigned long long)absolutePosition, (unsigned long long)state->position);
                return false;
            }

            remaining -= got;
        }

        stream.position = state->position;
        return true;
    }

    DebugLogWrite("WARN", "7Z", "STREAM_SEEK_FAIL", "index=%u | target=%llu | mode=%s | reason=unsupported_stream_mode", (unsigned int)state->index, (unsigned long long)absolutePosition, SevenZipStreamModeName(state->mode));
    return false;
}

void SevenZipCloseNonSolidEntryStreamImpl(ArchiveBackendStream& stream)
{
    if (stream.impl)
    {
        SevenZipNonSolidStreamState* state = static_cast<SevenZipNonSolidStreamState*>(stream.impl);
        CloseArchiveForStream(state);
        delete state;
    }

    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
}