/*
#define NOMINMAX
#include <windows.h>
#include "archive_rar_reader.h"
#include "archive_common.h"
#include "debug_log.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <new>
#include <sstream>
#include <string>
#include <vector>
#include <fstream>
#include <cwchar>
#include <cwctype>

#include "dll.hpp"

static char g_rarBackendPasswordListPath[MAX_PATH * 4] = "";

static INIT_ONCE g_rarBackendLockInitOnce = INIT_ONCE_STATIC_INIT;
static CRITICAL_SECTION g_rarBackendLock;

static BOOL CALLBACK RarInitBackendLock(PINIT_ONCE initOnce, PVOID parameter, PVOID* context)
{
    (void)initOnce;
    (void)parameter;
    (void)context;

    InitializeCriticalSection(&g_rarBackendLock);
    return TRUE;
}

static void RarEnterBackendLock()
{
    InitOnceExecuteOnce(&g_rarBackendLockInitOnce, RarInitBackendLock, NULL, NULL);
    EnterCriticalSection(&g_rarBackendLock);
}

static void RarLeaveBackendLock()
{
    LeaveCriticalSection(&g_rarBackendLock);
}

class RarBackendLockGuard
{
public:
    RarBackendLockGuard()
        : m_held(true)
    {
        RarEnterBackendLock();
    }

    ~RarBackendLockGuard()
    {
        if (m_held)
        {
            RarLeaveBackendLock();
        }
    }

    void Release()
    {
        if (m_held)
        {
            RarLeaveBackendLock();
            m_held = false;
        }
    }

    void Detach()
    {
        m_held = false;
    }

private:
    bool m_held;
};

static std::string WideToUtf8(const wchar_t* wide)
{
    if (!wide)
        return {};

    int needed = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);

    if (needed <= 0)
    {
        DebugLogWrite("WARN", "RAR", "WIDE_TO_UTF8_FAIL", "winerr=%lu | reason=size_query_failed", GetLastError());
        return {};
    }

    std::string out;
    out.resize((size_t)needed - 1);

    int converted = WideCharToMultiByte(CP_UTF8, 0, wide, -1, &out[0], needed, nullptr, nullptr);

    if (converted <= 0)
    {
        DebugLogWrite("WARN", "RAR", "WIDE_TO_UTF8_FAIL", "winerr=%lu | reason=convert_failed", GetLastError());
        return {};
    }

    return out;
}

static uint64_t RarMakeUInt64(unsigned int low, unsigned int high)
{
    return ((uint64_t)high << 32) | (uint64_t)low;
}

static bool RarArchivePathLooksMultipart(const wchar_t* archivePath)
{
    if (!archivePath || !archivePath[0])
    {
        return false;
    }

    const wchar_t* filename = wcsrchr(archivePath, L'\\');

    if (!filename)
    {
        filename = wcsrchr(archivePath, L'/');
    }

    filename = filename ? filename + 1 : archivePath;

    std::wstring lowerName = filename;

    for (wchar_t& ch : lowerName)
    {
        ch = (wchar_t)towlower(ch);
    }

    if (lowerName.find(L".part") != std::wstring::npos)
    {
        size_t len = lowerName.length();

        if (len >= 4 && lowerName.compare(len - 4, 4, L".rar") == 0)
        {
            return true;
        }
    }

    size_t dot = lowerName.find_last_of(L'.');

    if (dot != std::wstring::npos && dot + 4 == lowerName.length())
    {
        if (lowerName[dot + 1] == L'r' && iswdigit(lowerName[dot + 2]) && iswdigit(lowerName[dot + 3]))
        {
            return true;
        }
    }

    return false;
}

static std::string RarErrorToString(int code)
{
    switch (code)
    {
    case ERAR_SUCCESS: return "ERAR_SUCCESS";
    case ERAR_END_ARCHIVE: return "ERAR_END_ARCHIVE";
    case ERAR_NO_MEMORY: return "ERAR_NO_MEMORY";
    case ERAR_BAD_DATA: return "ERAR_BAD_DATA";
    case ERAR_BAD_ARCHIVE: return "ERAR_BAD_ARCHIVE";
    case ERAR_UNKNOWN_FORMAT: return "ERAR_UNKNOWN_FORMAT";
    case ERAR_EOPEN: return "ERAR_EOPEN";
    case ERAR_ECREATE: return "ERAR_ECREATE";
    case ERAR_ECLOSE: return "ERAR_ECLOSE";
    case ERAR_EREAD: return "ERAR_EREAD";
    case ERAR_EWRITE: return "ERAR_EWRITE";
    case ERAR_SMALL_BUF: return "ERAR_SMALL_BUF";
    case ERAR_UNKNOWN: return "ERAR_UNKNOWN";
    case ERAR_MISSING_PASSWORD: return "ERAR_MISSING_PASSWORD";
    case ERAR_EREFERENCE: return "ERAR_EREFERENCE";
    case ERAR_BAD_PASSWORD: return "ERAR_BAD_PASSWORD";
    case ERAR_LARGE_DICT: return "ERAR_LARGE_DICT";
    default:
    {
        std::ostringstream oss;
        oss << "UnRAR error " << code;
        return oss.str();
    }
    }
}

void RarSetPasswordListPath(const char* passwordListPath)
{
    if (!passwordListPath)
        passwordListPath = "";

    strcpy_s(g_rarBackendPasswordListPath, sizeof(g_rarBackendPasswordListPath), passwordListPath);
}

struct RarPasswordContext
{
    std::vector<std::string> passwords;
    size_t nextPassword = 0;
};

static void TrimPasswordLine(std::string& line)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();

    size_t start = 0;

    while (start < line.size() && (line[start] == ' ' || line[start] == '\t'))
        ++start;

    if (start > 0)
        line.erase(0, start);
}

static void LoadRarPasswords(RarPasswordContext& ctx)
{
    ctx.passwords.clear();
    ctx.nextPassword = 0;

    const char* passwordListPath = g_rarBackendPasswordListPath;

    
    //    archive_backend_test.exe does not load the Winamp config UI,
    //    so allow command-line/backend testing through an environment variable.
    
    if (!passwordListPath || !passwordListPath[0])
    {
        static char envPasswordListPath[MAX_PATH * 4];

        DWORD len = GetEnvironmentVariableA("ARCHIVE_STREAM_RAR_PASSWORD_LIST", envPasswordListPath, sizeof(envPasswordListPath));

        if (len > 0 && len < sizeof(envPasswordListPath))
            passwordListPath = envPasswordListPath;
    }

    if (!passwordListPath || !passwordListPath[0])
        return;

    std::ifstream file(passwordListPath, std::ios::binary);

    if (!file)
    {
        DebugLogWrite("WARN", "RAR", "PASSWORD_LIST_OPEN_FAIL", "path=%s", passwordListPath);
        return;
    }

    std::string line;

    while (std::getline(file, line))
    {
        TrimPasswordLine(line);

        if (line.empty())
            continue;

        if (line[0] == '#')
            continue;

        
        //    Strip UTF-8 BOM if present on first line.
        
        if (ctx.passwords.empty() && line.size() >= 3 && (unsigned char)line[0] == 0xEF && (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
        {
            line.erase(0, 3);
            TrimPasswordLine(line);

            if (line.empty())
                continue;
        }

        ctx.passwords.push_back(line);
    }
}

static int RarProvidePasswordA(RarPasswordContext& ctx, LPARAM p1, LPARAM p2)
{
    char* buffer = reinterpret_cast<char*>(p1);
    size_t bufferChars = (size_t)p2;

    if (!buffer || bufferChars == 0)
        return -1;

    if (ctx.nextPassword >= ctx.passwords.size())
        return -1;

    const std::string& password = ctx.passwords[ctx.nextPassword++];

    strcpy_s(buffer, bufferChars, password.c_str());
    return 1;
}

static int RarProvidePasswordW(RarPasswordContext& ctx, LPARAM p1, LPARAM p2)
{
    wchar_t* buffer = reinterpret_cast<wchar_t*>(p1);
    size_t bufferChars = (size_t)p2;

    if (!buffer || bufferChars == 0)
        return -1;

    if (ctx.nextPassword >= ctx.passwords.size())
        return -1;

    const std::string& password = ctx.passwords[ctx.nextPassword++];

    int needed = MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, nullptr, 0);

    if (needed <= 0)
        needed = MultiByteToWideChar(CP_ACP, 0, password.c_str(), -1, nullptr, 0);

    if (needed <= 0 || (size_t)needed > bufferChars)
        return -1;

    int written = MultiByteToWideChar(CP_UTF8, 0, password.c_str(), -1, buffer, (int)bufferChars);

    if (written <= 0)
        written = MultiByteToWideChar(CP_ACP, 0, password.c_str(), -1, buffer, (int)bufferChars);

    return written > 0 ? 1 : -1;
}

static bool RarApplyPasswordByIndex(HANDLE h, RarPasswordContext& ctx, size_t passwordIndex)
{
    if (!h)
        return false;

    if (passwordIndex >= ctx.passwords.size())
        return false;

    RARSetPassword(h, const_cast<char*>(ctx.passwords[passwordIndex].c_str()));

    ctx.nextPassword = passwordIndex + 1;
    return true;
}

static HANDLE RarOpen(const wchar_t* archivePath, unsigned int openMode, RAROpenArchiveDataEx* outOpenData, std::string& error)
{
    if (!archivePath || !*archivePath || !outOpenData)
    {
        error = "RAR open failed: archive path is empty";
        DebugLogWrite("ERROR", "RAR", "OPEN_FAIL", "mode=%u | reason=empty_path_or_open_data", openMode);
        return nullptr;
    }

    memset(outOpenData, 0, sizeof(*outOpenData));

    outOpenData->ArcNameW = const_cast<wchar_t*>(archivePath);
    outOpenData->OpenMode = openMode;

    HANDLE h = RAROpenArchiveEx(outOpenData);

    if (!h || outOpenData->OpenResult != ERAR_SUCCESS)
    {
        error = "RAR open failed: " + RarErrorToString(outOpenData->OpenResult);
        DebugLogWrite("ERROR", "RAR", "OPEN_FAIL", "mode=%u | result=%d | error=%s", openMode, outOpenData->OpenResult, error.c_str());
        return nullptr;
    }

    return h;
}

static bool RarPathEndsWithSlash(const RARHeaderDataEx& header)
{
    const wchar_t* nameW = header.FileNameW;

    if (nameW && nameW[0])
    {
        size_t len = wcslen(nameW);

        if (len > 0 && (nameW[len - 1] == L'\\' || nameW[len - 1] == L'/'))
        {
            return true;
        }
    }

    const char* nameA = header.FileName;

    if (nameA && nameA[0])
    {
        size_t len = strlen(nameA);

        if (len > 0 && (nameA[len - 1] == '\\' || nameA[len - 1] == '/'))
        {
            return true;
        }
    }

    return false;
}

static bool RarPathLooksLikeExtensionlessFolder(const std::string& path, uint64_t size)
{
    if (path.empty())
    {
        return false;
    }

    if (size != 0)
    {
        return false;
    }

    const size_t slash = path.find_last_of("\\/");
    const size_t dot = path.find_last_of('.');

    if (dot == std::string::npos)
    {
        return true;
    }

    if (slash != std::string::npos && dot < slash)
    {
        return true;
    }

    return false;
}

static bool RarIsDirectory(const RARHeaderDataEx& header, const std::string& normalizedPath)
{
    const uint64_t size = RarMakeUInt64(header.UnpSize, header.UnpSizeHigh);

    if ((header.Flags & RHDF_DIRECTORY) != 0)
    {
        if (size == 0 || !ArchiveIsSupportedAudioPath(normalizedPath.c_str()))
        {
            return true;
        }

        DebugLogWrite("WARN", "RAR", "DIR_FLAG_IGNORED_FOR_AUDIO", "path=%s | size=%llu | flags=0x%08X | file_attr=0x%08X", normalizedPath.c_str(), (unsigned long long)size, (unsigned int)header.Flags, (unsigned int)header.FileAttr);
        return false;
    }

    if (RarPathEndsWithSlash(header))
    {
        return true;
    }

    if (RarPathLooksLikeExtensionlessFolder(normalizedPath, size))
    {
        return true;
    }

    return false;
}

bool RarListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    RarBackendLockGuard backendLock;

    entries.clear();
    error.clear();

    const bool archiveIsMultipart = RarArchivePathLooksMultipart(archivePath);

    RAROpenArchiveDataEx openData;
    HANDLE h = RarOpen(archivePath, RAR_OM_LIST, &openData, error);

    if (!h)
    {
        DebugLogWrite("ERROR", "RAR", "LIST_FAIL", "reason=open_failed | error=%s", error.c_str());
        return false;
    }

    uint32_t rawIndex = 0;

    while (true)
    {
        RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));

        int headerResult = RARReadHeaderEx(h, &header);

        if (headerResult == ERAR_END_ARCHIVE)
            break;

        if (headerResult != ERAR_SUCCESS)
        {
            error = "RAR read header failed: " + RarErrorToString(headerResult);
            DebugLogWrite("ERROR", "RAR", "LIST_FAIL", "reason=read_header_failed | raw_index=%u | error=%s", (unsigned int)rawIndex, error.c_str());
            RARCloseArchive(h);
            return false;
        }

        std::string path = WideToUtf8(header.FileNameW);

        if (path.empty() && header.FileName[0])
        {
            path = header.FileName;
        }

        std::replace(path.begin(), path.end(), '/', '\\');

        const uint64_t entrySize = RarMakeUInt64(header.UnpSize, header.UnpSizeHigh);
        bool isFolder = RarIsDirectory(header, path);

        ArchiveEntry entry;
        entry.index = rawIndex;
        entry.path = path;
        entry.size = entrySize;
        entry.isFolder = isFolder;
        entry.isSupportedAudio = !entry.isFolder && ArchiveIsSupportedAudioPath(entry.path.c_str());

        const bool isSplitBefore = (header.Flags & RHDF_SPLITBEFORE) != 0;
        const bool isSplitAfter = (header.Flags & RHDF_SPLITAFTER) != 0;
        const bool isSplitEntry = isSplitBefore || isSplitAfter;

        const bool crcFromHashType = header.HashType == RAR_HASH_CRC32;
        const bool crcFromFileCrc = header.FileCRC != 0xFFFFFFFFu;
        const bool crcHeaderIsUsable = !archiveIsMultipart && !isSplitEntry && (crcFromHashType || crcFromFileCrc);

        entry.hasCrc32 = crcHeaderIsUsable;
        entry.crc32 = entry.hasCrc32 ? (uint32_t)header.FileCRC : 0;

        if (entry.isSupportedAudio)
        {
        //    DebugLogWrite("INFO", "CRC", "RAR_ENTRY_CRC", "raw_index=%u | path=%s | size=%llu | has_crc=%d | entry_crc=%08X | header_file_crc=%08X | hash_type=%u | crc_from_hash_type=%d | crc_from_file_crc=%d | split_before=%d | split_after=%d | crc_header_usable=%d | flags=0x%08X | file_attr=0x%08X | method=%u | solid=%d", (unsigned int)rawIndex, entry.path.c_str(), (unsigned long long)entry.size, entry.hasCrc32 ? 1 : 0, (unsigned int)entry.crc32, (unsigned int)header.FileCRC, (unsigned int)header.HashType, crcFromHashType ? 1 : 0, crcFromFileCrc ? 1 : 0, isSplitBefore ? 1 : 0, isSplitAfter ? 1 : 0, crcHeaderIsUsable ? 1 : 0, (unsigned int)header.Flags, (unsigned int)header.FileAttr, (unsigned int)header.Method, ((header.Flags & RHDF_SOLID) != 0) ? 1 : 0);
        //    DebugLogWrite("INFO", "CRC", "RAR_ENTRY_CRC", "raw_index=%u | path=%s | size=%llu | has_crc=%d | entry_crc=%08X | header_file_crc=%08X | hash_type=%u | crc_from_hash_type=%d | crc_from_file_crc=%d | flags=0x%08X | file_attr=0x%08X | method=%u | solid=%d", (unsigned int)rawIndex, entry.path.c_str(), (unsigned long long)entry.size, entry.hasCrc32 ? 1 : 0, (unsigned int)entry.crc32, (unsigned int)header.FileCRC, (unsigned int)header.HashType, crcFromHashType ? 1 : 0, crcFromFileCrc ? 1 : 0, (unsigned int)header.Flags, (unsigned int)header.FileAttr, (unsigned int)header.Method, ((header.Flags & RHDF_SOLID) != 0) ? 1 : 0);
        }

        if (entry.isFolder) // for debug only!
        {
            //DebugLogWrite("INFO", "RAR", "ENTRY_FOLDER", "raw_index=%u | path=%s | size=%llu | flags=0x%08X | file_attr=0x%08X", (unsigned int)rawIndex, entry.path.c_str(), (unsigned long long)entry.size, (unsigned int)header.Flags, (unsigned int)header.FileAttr);
        }
        else if (!entry.isSupportedAudio && rawIndex < 12)
        {
            //DebugLogWrite("INFO", "RAR", "ENTRY_NOT_AUDIO", "raw_index=%u | path=%s | size=%llu", (unsigned int)rawIndex, entry.path.c_str(), (unsigned long long)entry.size);
        }

        entry.isSolid = (header.Flags & RHDF_SOLID) != 0;
        entry.folderIndex = 0xFFFFFFFF;
        entry.filesInFolder = 0;
        entry.methodId = header.Method;
        entry.hasMethodId = true;

        
        //    RAR streaming first pass:
        //    - non-solid files only
        //    - no directories
        //    - supported audio only
        //    - password/missing-volume cases still fail cleanly at stream open/read
        
        entry.streamCandidate = entry.isSupportedAudio && !entry.isFolder && !entry.isSolid;

        try
        {
            entries.push_back(entry);
        }
        catch (const std::bad_alloc&)
        {
            error = "RAR list failed: not enough memory to add entry";
            DebugLogWrite("ERROR", "RAR", "LIST_FAIL", "reason=push_entry_failed | raw_index=%u | path=%s", (unsigned int)rawIndex, entry.path.c_str());
            RARCloseArchive(h);
            return false;
        }

        int processResult = RARProcessFileW(h, RAR_SKIP, nullptr, nullptr);

        if (processResult != ERAR_SUCCESS)
        {
            error = "RAR skip failed: " + RarErrorToString(processResult);
            DebugLogWrite("ERROR", "RAR", "LIST_FAIL", "reason=skip_failed | raw_index=%u | path=%s | error=%s", (unsigned int)rawIndex, entry.path.c_str(), error.c_str());
            RARCloseArchive(h);
            return false;
        }

        ++rawIndex;
    }

    bool archiveHasSolidEntries = false;

    for (const ArchiveEntry& e : entries)
    {
        if (e.isSolid)
        {
            archiveHasSolidEntries = true;
            break;
        }
    }

    if (archiveHasSolidEntries)
    {
        for (ArchiveEntry& e : entries)
        {
            e.isSolid = true;
            e.streamCandidate = false;
        }
    }

    RARCloseArchive(h);
    return true;
}

static bool RarVolumeFileExistsA(const char* path)
{
    if (!path || !path[0])
        return false;

    DWORD attr = GetFileAttributesA(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static bool RarVolumeFileExistsW(const wchar_t* path)
{
    if (!path || !path[0])
        return false;

    DWORD attr = GetFileAttributesW(path);
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

static int RarHandleChangeVolumeA(LPARAM p1, LPARAM)
{
    const char* volumePath = reinterpret_cast<const char*>(p1);
    return RarVolumeFileExistsA(volumePath) ? 1 : -1;
}

static int RarHandleChangeVolumeW(LPARAM p1, LPARAM)
{
    const wchar_t* volumePath = reinterpret_cast<const wchar_t*>(p1);
    return RarVolumeFileExistsW(volumePath) ? 1 : -1;
}

struct RarFileCallbackContext
{
    RarPasswordContext passwordContext;
};

static int CALLBACK RarFileCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
    RarFileCallbackContext* ctx = reinterpret_cast<RarFileCallbackContext*>(userData);

    if (msg == UCM_CHANGEVOLUME)
        return RarHandleChangeVolumeA(p1, p2);

    if (msg == UCM_CHANGEVOLUMEW)
        return RarHandleChangeVolumeW(p1, p2);

    if (msg == UCM_NEEDPASSWORD)
    {
        if (!ctx)
            return -1;

        return RarProvidePasswordA(ctx->passwordContext, p1, p2);
    }

    if (msg == UCM_NEEDPASSWORDW)
    {
        if (!ctx)
            return -1;

        return RarProvidePasswordW(ctx->passwordContext, p1, p2);
    }

    if (msg == UCM_LARGEDICT)
        return -1;

    if (msg == UCM_PROCESSDATA)
        return 1;

    return 1;
}

struct RarMemoryContext
{
    std::vector<unsigned char>* data = nullptr;
    std::string* error = nullptr;
    bool captureData = false;
    RarPasswordContext passwordContext;
};

static bool RarArchiveHasSolidEntries(const wchar_t* archivePath)
{
    std::vector<ArchiveEntry> entries;
    std::string error;

    if (!RarListEntries(archivePath, entries, error))
        return false;

    for (const ArchiveEntry& e : entries)
    {
        if (e.isSolid)
            return true;
    }

    return false;
}

static void RarNormalizePathForCompare(std::string& path)
{
    for (char& c : path)
    {
        if (c == '/')
            c = '\\';
    }
}

static bool RarGetListedPathForIndex(const wchar_t* archivePath, uint32_t index, std::string& outPath, std::string& error)
{
    outPath.clear();

    std::vector<ArchiveEntry> entries;

    if (!RarListEntries(archivePath, entries, error))
    {
        DebugLogWrite("ERROR", "RAR", "ENTRY_LOOKUP_FAIL", "index=%u | reason=list_entries_failed | error=%s", (unsigned int)index, error.c_str());
        return false;
    }

    for (const ArchiveEntry& e : entries)
    {
        if (e.index == index)
        {
            outPath = e.path;
            RarNormalizePathForCompare(outPath);
            return true;
        }
    }

    error = "RAR entry index not found in listing";
    DebugLogWrite("ERROR", "RAR", "ENTRY_LOOKUP_FAIL", "index=%u | entries=%zu | reason=index_not_found", (unsigned int)index, entries.size());
    return false;
}

static std::string RarHeaderPathToUtf8(const RARHeaderDataEx& header)
{
    std::string path = WideToUtf8(header.FileNameW);

    if (path.empty() && header.FileName[0])
        path = header.FileName;

    RarNormalizePathForCompare(path);
    return path;
}

static bool RarHeaderMatchesListedPath(const RARHeaderDataEx& header, const std::string& listedPath)
{
    std::string headerPath = RarHeaderPathToUtf8(header);

    if (headerPath.empty() || listedPath.empty())
        return false;

    return _stricmp(headerPath.c_str(), listedPath.c_str()) == 0;
}

static int CALLBACK RarMemoryCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
    RarMemoryContext* ctx = reinterpret_cast<RarMemoryContext*>(userData);

    if (!ctx)
        return -1;

    if (msg == UCM_PROCESSDATA)
    {
        if (!ctx->captureData)
            return 1;

        if (!ctx->data)
            return -1;

        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(p1);
        int size = (int)p2;

        if (!bytes || size < 0)
            return -1;

        try
        {
            ctx->data->insert(ctx->data->end(), bytes, bytes + size);
        }
        catch (...)
        {
            if (ctx->error)
                *ctx->error = "RAR memory extraction failed: not enough memory";

            DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "reason=callback_insert_failed | bytes=%d", size);
            return -1;
        }

        return 1;
    }

    if (msg == UCM_CHANGEVOLUME)
        return RarHandleChangeVolumeA(p1, p2);

    if (msg == UCM_CHANGEVOLUMEW)
        return RarHandleChangeVolumeW(p1, p2);

    if (msg == UCM_NEEDPASSWORD)
        return RarProvidePasswordA(ctx->passwordContext, p1, p2);

    if (msg == UCM_NEEDPASSWORDW)
        return RarProvidePasswordW(ctx->passwordContext, p1, p2);

    if (msg == UCM_LARGEDICT)
        return -1;

    return 1;
}

static bool RarExtractEntryToMemoryAttempt(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, const std::string* password, std::string& error)
{
    data.clear();
    error.clear();

    std::string targetPath;

    if (!RarGetListedPathForIndex(archivePath, index, targetPath, error))
        return false;

    bool archiveHasSolidEntries = RarArchiveHasSolidEntries(archivePath);

    RAROpenArchiveDataEx openData;
    HANDLE h = RarOpen(archivePath, RAR_OM_EXTRACT, &openData, error);

    if (!h)
    {
        DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | reason=open_failed | error=%s", (unsigned int)index, error.c_str());
        return false;
    }

    RarMemoryContext ctx;
    ctx.data = &data;
    ctx.error = &error;
    ctx.captureData = false;

    LoadRarPasswords(ctx.passwordContext);

    if (password && !password->empty())
        RARSetPassword(h, const_cast<char*>(password->c_str()));

    RARSetCallback(h, RarMemoryCallback, (LPARAM)&ctx);

    while (true)
    {
        RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));

        int headerResult = RARReadHeaderEx(h, &header);

        if (headerResult == ERAR_END_ARCHIVE)
        {
            error = "RAR extract failed: index not found";
            DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | target=%s | reason=index_not_found", (unsigned int)index, targetPath.c_str());
            RARCloseArchive(h);
            return false;
        }

        if (headerResult != ERAR_SUCCESS)
        {
            error = "RAR read header failed during extract: " + RarErrorToString(headerResult);
            DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | target=%s | reason=read_header_failed | error=%s", (unsigned int)index, targetPath.c_str(), error.c_str());
            RARCloseArchive(h);
            return false;
        }

        if (RarHeaderMatchesListedPath(header, targetPath))
        {
            if (RarIsDirectory(header, targetPath))
            {
                error = "RAR extract failed: selected index is a folder";
                DebugLogWrite("WARN", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | target=%s | reason=index_is_folder", (unsigned int)index, targetPath.c_str());
                RARCloseArchive(h);
                return false;
            }

            ctx.captureData = true;

            int processResult = RARProcessFileW(h, RAR_TEST, nullptr, nullptr);

            if (processResult != ERAR_SUCCESS)
            {
                if (error.empty())
                    error = "RAR memory extraction failed: " + RarErrorToString(processResult);

                DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | target=%s | result=%d | error=%s", (unsigned int)index, targetPath.c_str(), processResult, error.c_str());
                RARCloseArchive(h);
                return false;
            }

            RARCloseArchive(h);
            return true;
        }

        int processResult = RARProcessFileW(h, archiveHasSolidEntries ? RAR_TEST : RAR_SKIP, nullptr, nullptr);

        if (processResult != ERAR_SUCCESS)
        {
            error = "RAR skip failed during extract: " + RarErrorToString(processResult);
            DebugLogWrite("ERROR", "RAR", "MEMORY_EXTRACT_FAIL", "index=%u | target=%s | reason=skip_failed | error=%s", (unsigned int)index, targetPath.c_str(), error.c_str());
            RARCloseArchive(h);
            return false;
        }
    }
}

bool RarExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    RarBackendLockGuard backendLock;

    data.clear();
    error.clear();

    RarPasswordContext passwords;
    LoadRarPasswords(passwords);

    
    //    First try without a password, for normal unencrypted archives.
    
    if (RarExtractEntryToMemoryAttempt(archivePath, index, data, nullptr, error))
        return true;

    std::string firstError = error;
    size_t attempt = 0;

    for (const std::string& password : passwords.passwords)
    {
        if (password.empty())
            continue;

        ++attempt;

        if (RarExtractEntryToMemoryAttempt(archivePath, index, data, &password, error))
            return true;

        if (error.find("ERAR_BAD_PASSWORD") == std::string::npos && error.find("ERAR_MISSING_PASSWORD") == std::string::npos)
            return false;
    }

    if (!firstError.empty())
        error = firstError;

    DebugLogWrite("ERROR", "RAR", "PASSWORD_FAIL", "index=%u | attempts=%u | route=memory | error=%s", (unsigned int)index, (unsigned int)attempt, error.c_str());
    return false;
}

static bool RarExtractEntryToFileAttempt(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, const std::string* password, std::string& error)
{
    error.clear();

    std::string targetPath;

    if (!RarGetListedPathForIndex(archivePath, index, targetPath, error))
        return false;

    bool archiveHasSolidEntries = RarArchiveHasSolidEntries(archivePath);

    if (!outputPath || !*outputPath)
    {
        error = "RAR extract-to-file failed: output path is empty";
        DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=empty_output_path", (unsigned int)index, targetPath.c_str());
        return false;
    }

    RAROpenArchiveDataEx openData;
    HANDLE h = RarOpen(archivePath, RAR_OM_EXTRACT, &openData, error);

    if (!h)
    {
        DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=open_failed | error=%s", (unsigned int)index, targetPath.c_str(), error.c_str());
        return false;
    }

    RarFileCallbackContext fileCallbackCtx;
    LoadRarPasswords(fileCallbackCtx.passwordContext);

    if (password && !password->empty())
        RARSetPassword(h, const_cast<char*>(password->c_str()));

    RARSetCallback(h, RarFileCallback, (LPARAM)&fileCallbackCtx);

    while (true)
    {
        RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));

        int headerResult = RARReadHeaderEx(h, &header);

        if (headerResult == ERAR_END_ARCHIVE)
        {
            error = "RAR extract-to-file failed: index not found";
            DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=index_not_found", (unsigned int)index, targetPath.c_str());
            RARCloseArchive(h);
            return false;
        }

        if (headerResult != ERAR_SUCCESS)
        {
            error = "RAR read header failed during extract-to-file: " + RarErrorToString(headerResult);
            DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=read_header_failed | error=%s", (unsigned int)index, targetPath.c_str(), error.c_str());
            RARCloseArchive(h);
            return false;
        }

        if (RarHeaderMatchesListedPath(header, targetPath))
        {
            if (RarIsDirectory(header, targetPath))
            {
                error = "RAR extract-to-file failed: selected index is a folder";
                DebugLogWrite("WARN", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=index_is_folder", (unsigned int)index, targetPath.c_str());
                RARCloseArchive(h);
                return false;
            }

            DeleteFileW(outputPath);

            int processResult = RARProcessFileW(h, RAR_EXTRACT, nullptr, const_cast<wchar_t*>(outputPath));

            if (processResult != ERAR_SUCCESS)
            {
                DeleteFileW(outputPath);

                error = "RAR extract-to-file failed: " + RarErrorToString(processResult);
                DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | result=%d | error=%s", (unsigned int)index, targetPath.c_str(), processResult, error.c_str());
                RARCloseArchive(h);
                return false;
            }

            RARCloseArchive(h);
            return true;
        }

        int processResult = RARProcessFileW(h, archiveHasSolidEntries ? RAR_TEST : RAR_SKIP, nullptr, nullptr);

        if (processResult != ERAR_SUCCESS)
        {
            error = "RAR skip failed during extract-to-file: " + RarErrorToString(processResult);
            DebugLogWrite("ERROR", "RAR", "FILE_EXTRACT_FAIL", "index=%u | target=%s | reason=skip_failed | error=%s", (unsigned int)index, targetPath.c_str(), error.c_str());
            RARCloseArchive(h);
            return false;
        }
    }
}

bool RarExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    RarBackendLockGuard backendLock;

    error.clear();

    RarPasswordContext passwords;
    LoadRarPasswords(passwords);

    
    //    First try without a password, for normal unencrypted archives.
    
    if (RarExtractEntryToFileAttempt(archivePath, index, outputPath, nullptr, error))
        return true;

    std::string firstError = error;
    size_t attempt = 0;

    for (const std::string& password : passwords.passwords)
    {
        if (password.empty())
            continue;

        ++attempt;

        if (RarExtractEntryToFileAttempt(archivePath, index, outputPath, &password, error))
            return true;
    }

    if (!firstError.empty())
        error = firstError;

    DebugLogWrite("ERROR", "RAR", "PASSWORD_FAIL", "index=%u | attempts=%u | route=file | error=%s", (unsigned int)index, (unsigned int)attempt, error.c_str());
    return false;
}

struct RarStreamState
{
    wchar_t archivePath[MAX_PATH * 4] = { 0 };
    uint32_t index = 0;
    char targetPath[1024] = { 0 };
    RarPasswordContext passwordContext;
    uint64_t size = 0;
    uint64_t position = 0;

    HANDLE thread = NULL;
    HANDLE stopEvent = NULL;
    HANDLE dataEvent = NULL;
    HANDLE spaceEvent = NULL;

    CRITICAL_SECTION lock;
    bool lockInitialized = false;

    std::vector<unsigned char> ring;
    size_t readPos = 0;
    size_t writePos = 0;
    size_t buffered = 0;

    bool eof = false;
    bool failed = false;
    std::string error;
};

static const size_t RAR_STREAM_RING_SIZE = 4 * 1024 * 1024;

static size_t RarStreamSpaceLocked(const RarStreamState* state)
{
    if (!state)
        return 0;

    return state->ring.size() - state->buffered;
}

static void RarStreamUpdateEventsLocked(RarStreamState* state)
{
    if (!state)
        return;

    if (state->buffered > 0 || state->eof || state->failed)
        SetEvent(state->dataEvent);
    else
        ResetEvent(state->dataEvent);

    if (RarStreamSpaceLocked(state) > 0)
        SetEvent(state->spaceEvent);
    else
        ResetEvent(state->spaceEvent);
}

static bool RarStreamWrite(RarStreamState* state, const unsigned char* data, size_t dataSize)
{
    if (!state || !data || dataSize == 0)
        return true;

    size_t written = 0;

    while (written < dataSize)
    {
        if (WaitForSingleObject(state->stopEvent, 0) == WAIT_OBJECT_0)
            return false;

        EnterCriticalSection(&state->lock);

        size_t space = RarStreamSpaceLocked(state);

        if (space > 0)
        {
            size_t toWrite = dataSize - written;

            if (toWrite > space)
                toWrite = space;

            size_t contiguous = state->ring.size() - state->writePos;

            if (toWrite > contiguous)
                toWrite = contiguous;

            memcpy(state->ring.data() + state->writePos, data + written, toWrite);

            state->writePos = (state->writePos + toWrite) % state->ring.size();
            state->buffered += toWrite;
            written += toWrite;

            RarStreamUpdateEventsLocked(state);
            LeaveCriticalSection(&state->lock);
            continue;
        }

        RarStreamUpdateEventsLocked(state);
        LeaveCriticalSection(&state->lock);

        HANDLE waits[2] = { state->stopEvent, state->spaceEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0)
            return false;
    }

    return true;
}

static void RarStreamSetFinished(RarStreamState* state, bool failed, const std::string& error)
{
    if (!state)
        return;

    EnterCriticalSection(&state->lock);

    state->failed = failed;
    state->error = error;
    state->eof = true;

    RarStreamUpdateEventsLocked(state);
    LeaveCriticalSection(&state->lock);
}

static int CALLBACK RarStreamCallback(UINT msg, LPARAM userData, LPARAM p1, LPARAM p2)
{
    RarStreamState* state = reinterpret_cast<RarStreamState*>(userData);

    if (!state)
        return -1;

    if (WaitForSingleObject(state->stopEvent, 0) == WAIT_OBJECT_0)
        return -1;

    if (msg == UCM_PROCESSDATA)
    {
        const unsigned char* bytes = reinterpret_cast<const unsigned char*>(p1);
        int size = (int)p2;

        if (!bytes || size < 0)
            return -1;

        return RarStreamWrite(state, bytes, (size_t)size) ? 1 : -1;
    }

    if (msg == UCM_CHANGEVOLUME)
        return RarHandleChangeVolumeA(p1, p2);

    if (msg == UCM_CHANGEVOLUMEW)
        return RarHandleChangeVolumeW(p1, p2);

    if (msg == UCM_NEEDPASSWORD)
        return RarProvidePasswordA(state->passwordContext, p1, p2);

    if (msg == UCM_NEEDPASSWORDW)
        return RarProvidePasswordW(state->passwordContext, p1, p2);

    if (msg == UCM_LARGEDICT)
        return -1;

    return 1;
}

static DWORD WINAPI RarStreamThreadProc(LPVOID param)
{
    RarStreamState* state = reinterpret_cast<RarStreamState*>(param);

    if (!state)
    {
        return 1;
    }

    std::string error;

    RAROpenArchiveDataEx openData;
    HANDLE h = RarOpen(state->archivePath, RAR_OM_EXTRACT, &openData, error);

    if (!h)
    {
        DebugLogWrite("ERROR", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=open_failed | error=%s", (unsigned int)state->index, state->targetPath, error.c_str());
        RarStreamSetFinished(state, true, error);
        return 1;
    }

    //if (!state->passwordContext.passwords.empty())
    //    RarApplyPasswordByIndex(h, state->passwordContext, 0);

    RARSetCallback(h, RarStreamCallback, (LPARAM)state);

    while (true)
    {
        if (WaitForSingleObject(state->stopEvent, 0) == WAIT_OBJECT_0)
        {
            RARCloseArchive(h);
            RarStreamSetFinished(state, false, "RAR stream stopped");
            return 0;
        }

        RARHeaderDataEx header;
        memset(&header, 0, sizeof(header));

        int headerResult = RARReadHeaderEx(h, &header);

        if (headerResult == ERAR_END_ARCHIVE)
        {
            std::string err = "RAR stream failed: index not found";
            DebugLogWrite("ERROR", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=index_not_found", (unsigned int)state->index, state->targetPath);
            RARCloseArchive(h);
            RarStreamSetFinished(state, true, err);
            return 1;
        }

        if (headerResult != ERAR_SUCCESS)
        {
            std::string err = "RAR stream read header failed: " + RarErrorToString(headerResult);
            DebugLogWrite("ERROR", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=read_header_failed | error=%s", (unsigned int)state->index, state->targetPath, err.c_str());
            RARCloseArchive(h);
            RarStreamSetFinished(state, true, err);
            return 1;
        }

        if (RarHeaderMatchesListedPath(header, state->targetPath))
        {
            if (RarIsDirectory(header, state->targetPath))
            {
                std::string err = "RAR stream failed: selected index is a folder";
                DebugLogWrite("WARN", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=index_is_folder", (unsigned int)state->index, state->targetPath);
                RARCloseArchive(h);
                RarStreamSetFinished(state, true, err);
                return 1;
            }

            if (header.Flags & RHDF_SOLID)
            {
                std::string err = "RAR stream failed: solid RAR entries use cache fallback";
                DebugLogWrite("WARN", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=solid_entry", (unsigned int)state->index, state->targetPath);
                RARCloseArchive(h);
                RarStreamSetFinished(state, true, err);
                return 1;
            }

            int processResult = RARProcessFileW(h, RAR_TEST, nullptr, nullptr);

            if (processResult != ERAR_SUCCESS)
            {
                if (WaitForSingleObject(state->stopEvent, 0) == WAIT_OBJECT_0)
                {
                    RARCloseArchive(h);
                    RarStreamSetFinished(state, false, "RAR stream stopped");
                    return 0;
                }

                std::string err = "RAR stream extraction failed: " + RarErrorToString(processResult);
                DebugLogWrite("ERROR", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | result=%d | error=%s | header_flags=0x%08X | password_count=%zu", (unsigned int)state->index, state->targetPath, processResult, err.c_str(), (unsigned int)header.Flags, state->passwordContext.passwords.size());
                RARCloseArchive(h);
                RarStreamSetFinished(state, true, err);
                return 1;
            }

            RARCloseArchive(h);
            RarStreamSetFinished(state, false, "");
            return 0;
        }

        int processResult = RARProcessFileW(h, RAR_SKIP, nullptr, nullptr);

        if (processResult != ERAR_SUCCESS)
        {
            if (WaitForSingleObject(state->stopEvent, 0) == WAIT_OBJECT_0)
            {
                RARCloseArchive(h);
                RarStreamSetFinished(state, false, "RAR stream stopped");
                return 0;
            }

            std::string err = "RAR stream skip failed: " + RarErrorToString(processResult);
            DebugLogWrite("ERROR", "RAR", "STREAM_THREAD_FAIL", "index=%u | target=%s | reason=skip_failed | error=%s", (unsigned int)state->index, state->targetPath, err.c_str());
            RARCloseArchive(h);
            RarStreamSetFinished(state, true, err);
            return 1;
        }
    }
}

static void RarStopWorker(RarStreamState* state)
{
    if (!state)
        return;

    if (state->stopEvent)
        SetEvent(state->stopEvent);

    if (state->thread)
    {
        WaitForSingleObject(state->thread, INFINITE);
        CloseHandle(state->thread);
        state->thread = NULL;
    }
}

static bool RarStartWorker(RarStreamState* state)
{
    if (!state)
        return false;

    EnterCriticalSection(&state->lock);

    state->readPos = 0;
    state->writePos = 0;
    state->buffered = 0;
    state->eof = false;
    state->failed = false;
    state->error.clear();

    ResetEvent(state->stopEvent);
    ResetEvent(state->dataEvent);
    SetEvent(state->spaceEvent);

    LeaveCriticalSection(&state->lock);

    state->thread = CreateThread(NULL, 0, RarStreamThreadProc, state, 0, NULL);

    if (!state->thread)
        DebugLogWrite("ERROR", "RAR", "STREAM_START_FAIL", "index=%u | target=%s | winerr=%lu | reason=create_thread_failed", (unsigned int)state->index, state->targetPath, GetLastError());

    return state->thread != NULL;
}

static bool RarCreateStreamState(const wchar_t* archivePath, uint32_t index, RarStreamState** outState, std::string& error)
{
    if (outState)
        *outState = nullptr;

    if (!outState || !archivePath || !*archivePath)
    {
        error = "RAR stream open failed: archive path is empty";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | reason=empty_archive_path", (unsigned int)index);
        return false;
    }

    std::vector<ArchiveEntry> entries;

    if (!RarListEntries(archivePath, entries, error))
    {
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | reason=list_entries_failed | error=%s", (unsigned int)index, error.c_str());
        return false;
    }

    const ArchiveEntry* selected = nullptr;

    for (const ArchiveEntry& e : entries)
    {
        if (e.index == index)
        {
            selected = &e;
            break;
        }
    }

    if (!selected)
    {
        error = "RAR stream open failed: index not found";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | entries=%zu | reason=index_not_found", (unsigned int)index, entries.size());
        return false;
    }

    if (selected->isFolder)
    {
        error = "RAR stream open failed: selected index is a folder";
        DebugLogWrite("WARN", "RAR", "STREAM_OPEN_FAIL", "index=%u | path=%s | reason=index_is_folder", (unsigned int)index, selected->path.c_str());
        return false;
    }

    if (selected->isSolid)
    {
        error = "RAR stream open failed: solid RAR entries use cache fallback";
        DebugLogWrite("WARN", "RAR", "STREAM_OPEN_FAIL", "index=%u | path=%s | reason=solid_entry", (unsigned int)index, selected->path.c_str());
        return false;
    }

    RarStreamState* state = new (std::nothrow) RarStreamState();

    if (!state)
    {
        error = "RAR stream open failed: not enough memory";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | reason=state_alloc_failed", (unsigned int)index);
        return false;
    }

    wcsncpy_s(state->archivePath, sizeof(state->archivePath) / sizeof(state->archivePath[0]), archivePath, _TRUNCATE);

    state->index = index;
    state->size = selected->size;
    state->position = 0;

    std::string selectedPath = selected->path;
    RarNormalizePathForCompare(selectedPath);

    strcpy_s(state->targetPath, sizeof(state->targetPath), selectedPath.c_str());

    LoadRarPasswords(state->passwordContext);

    InitializeCriticalSection(&state->lock);
    state->lockInitialized = true;

    try
    {
        state->ring.resize(RAR_STREAM_RING_SIZE);
    }
    catch (...)
    {
        DeleteCriticalSection(&state->lock);
        delete state;
        error = "RAR stream open failed: could not allocate ring buffer";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | bytes=%zu | reason=ring_alloc_failed", (unsigned int)index, RAR_STREAM_RING_SIZE);
        return false;
    }

    state->stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    state->dataEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    state->spaceEvent = CreateEventW(NULL, TRUE, TRUE, NULL);

    if (!state->stopEvent || !state->dataEvent || !state->spaceEvent)
    {
        DWORD err = GetLastError();

        if (state->stopEvent)
            CloseHandle(state->stopEvent);

        if (state->dataEvent)
            CloseHandle(state->dataEvent);

        if (state->spaceEvent)
            CloseHandle(state->spaceEvent);

        DeleteCriticalSection(&state->lock);
        delete state;

        error = "RAR stream open failed: could not create events";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | winerr=%lu | reason=create_events_failed", (unsigned int)index, err);
        return false;
    }

    if (!RarStartWorker(state))
    {
        CloseHandle(state->stopEvent);
        CloseHandle(state->dataEvent);
        CloseHandle(state->spaceEvent);
        DeleteCriticalSection(&state->lock);
        delete state;

        error = "RAR stream open failed: could not start worker thread";
        DebugLogWrite("ERROR", "RAR", "STREAM_OPEN_FAIL", "index=%u | reason=start_worker_failed", (unsigned int)index);
        return false;
    }

    *outState = state;
    return true;
}

static void RarDestroyStreamState(RarStreamState* state)
{
    if (!state)
        return;

    RarStopWorker(state);

    if (state->stopEvent)
        CloseHandle(state->stopEvent);

    if (state->dataEvent)
        CloseHandle(state->dataEvent);

    if (state->spaceEvent)
        CloseHandle(state->spaceEvent);

    if (state->lockInitialized)
        DeleteCriticalSection(&state->lock);

    delete state;
}

bool RarOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error)
{
    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Rar;
    error.clear();

    RarStreamState* state = nullptr;

    if (!RarCreateStreamState(archivePath, index, &state, error))
    {
        return false;
    }

    stream.impl = state;
    stream.size = state->size;
    stream.position = 0;
    stream.format = ArchiveFormat::Rar;

    DebugLogWrite("INFO", "RAR", "STREAM_OPEN_OK", "index=%u | size=%llu", (unsigned int)index, (unsigned long long)state->size);
    return true;
}

size_t RarReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead)
{
    if (!stream.impl || !buffer || bytesToRead == 0)
        return 0;

    RarStreamState* state = static_cast<RarStreamState*>(stream.impl);
    unsigned char* out = static_cast<unsigned char*>(buffer);

    size_t totalRead = 0;

    while (totalRead < bytesToRead)
    {
        EnterCriticalSection(&state->lock);

        if (state->buffered > 0)
        {
            size_t toRead = bytesToRead - totalRead;

            if (toRead > state->buffered)
                toRead = state->buffered;

            size_t contiguous = state->ring.size() - state->readPos;

            if (toRead > contiguous)
                toRead = contiguous;

            memcpy(out + totalRead, state->ring.data() + state->readPos, toRead);

            state->readPos = (state->readPos + toRead) % state->ring.size();
            state->buffered -= toRead;
            state->position += toRead;
            totalRead += toRead;

            RarStreamUpdateEventsLocked(state);
            LeaveCriticalSection(&state->lock);
            continue;
        }

        bool done = state->eof || state->failed;
        RarStreamUpdateEventsLocked(state);

        LeaveCriticalSection(&state->lock);

        if (done)
            break;

        HANDLE waits[2] = { state->stopEvent, state->dataEvent };
        DWORD waitResult = WaitForMultipleObjects(2, waits, FALSE, INFINITE);

        if (waitResult == WAIT_OBJECT_0)
            break;
    }

    stream.position = state->position;

    if (totalRead == 0 && bytesToRead > 0)
    {
        bool failed = false;
        bool eof = false;
        std::string error;

        EnterCriticalSection(&state->lock);

        failed = state->failed;
        eof = state->eof;
        error = state->error;

        LeaveCriticalSection(&state->lock);

        if (failed)
            DebugLogWrite("ERROR", "RAR", "STREAM_READ_FAIL", "index=%u | position=%llu | requested=%zu | error=%s", (unsigned int)state->index, (unsigned long long)state->position, bytesToRead, error.c_str());
        else if (!eof)
            DebugLogWrite("WARN", "RAR", "STREAM_READ_ZERO", "index=%u | position=%llu | requested=%zu | eof=no", (unsigned int)state->index, (unsigned long long)state->position, bytesToRead);
    }

    return totalRead;
}

bool RarSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    if (!stream.impl)
    {
        DebugLogWrite("ERROR", "RAR", "STREAM_SEEK_FAIL", "position=%llu | reason=no_stream_impl", (unsigned long long)absolutePosition);
        return false;
    }

    RarStreamState* state = static_cast<RarStreamState*>(stream.impl);

    if (absolutePosition > state->size)
    {
        DebugLogWrite("WARN", "RAR", "STREAM_SEEK_FAIL", "index=%u | target=%llu | size=%llu | reason=beyond_end", (unsigned int)state->index, (unsigned long long)absolutePosition, (unsigned long long)state->size);
        return false;
    }

    if (absolutePosition == state->position)
        return true;

    
    //    Forward seek:
    //    keep current worker alive and discard decoded bytes.
    
    if (absolutePosition > state->position)
    {
        unsigned char temp[64 * 1024];
        uint64_t remaining = absolutePosition - state->position;

        while (remaining > 0)
        {
            size_t want = remaining > sizeof(temp) ? sizeof(temp) : (size_t)remaining;
            size_t got = RarReadEntryStream(stream, temp, want);

            if (got == 0)
            {
                DebugLogWrite("WARN", "RAR", "STREAM_SEEK_FAIL", "index=%u | target=%llu | current=%llu | reason=discard_read_zero", (unsigned int)state->index, (unsigned long long)absolutePosition, (unsigned long long)state->position);
                return false;
            }

            remaining -= got;
        }

        stream.position = state->position;
        return true;
    }

    
    //    Backward seek:
    //    restart extraction from the beginning of this entry,
    //    then discard forward to the requested position.
    
    RarStopWorker(state);

    EnterCriticalSection(&state->lock);

    state->position = 0;
    state->readPos = 0;
    state->writePos = 0;
    state->buffered = 0;
    state->eof = false;
    state->failed = false;
    state->error.clear();

    RarStreamUpdateEventsLocked(state);

    LeaveCriticalSection(&state->lock);

    if (!RarStartWorker(state))
    {
        DebugLogWrite("ERROR", "RAR", "STREAM_SEEK_FAIL", "index=%u | target=%llu | reason=restart_worker_failed", (unsigned int)state->index, (unsigned long long)absolutePosition);
        return false;
    }

    stream.position = 0;

    if (absolutePosition == 0)
        return true;

    return RarSeekEntryStream(stream, absolutePosition);
}

void RarCloseEntryStream(ArchiveBackendStream& stream)
{
    if (stream.impl)
    {
        RarStreamState* state = static_cast<RarStreamState*>(stream.impl);
        RarDestroyStreamState(state);
    }

    stream.impl = nullptr;
    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Unknown;
}
*/

#define NOMINMAX

#include "archive_rar_reader.h"
#include "archive_rar_libarchive.h"
#include "archive_rar_native_encrypted.h"
#include "archive_common.h"
#include "archive_passwords.h"
#include "debug_log.h"

#include <windows.h>
#include <algorithm>
#include <cwctype>
#include <utility>

void RarSetPasswordListPath(const char* passwordListPath)
{
    ArchivePasswordSetListPath(passwordListPath);
}

static bool RarLooksLikeNumberedFirstVolume(const wchar_t* archivePath)
{
    if (!archivePath || !*archivePath)
        return false;

    std::wstring path(archivePath);
    std::transform(path.begin(), path.end(), path.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    const auto part = path.rfind(L".part1.rar");
    const bool partFirst = part != std::wstring::npos && part + 10 == path.size();
    const bool numericFirst = path.size() >= 8 && path.compare(path.size() - 8, 8, L".rar.001") == 0;
    return partFirst || numericFirst;
}

bool RarListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error)
{
    const bool listed = LibarchiveRarListEntries(archivePath, entries, error);
    if (listed && !entries.empty())
    {
        if (RarLooksLikeNumberedFirstVolume(archivePath))
        {
            std::string enrichmentError;
            if (!RarNativeEnrichMultipartEntryEncryption(archivePath, entries, enrichmentError))
                DebugLogWrite("WARN", "RAR", "MULTIPART_ENCRYPTION_METADATA_UNAVAILABLE", "entries=%zu | error=%s", entries.size(), enrichmentError.c_str());
        }
        return true;
    }

    // RAR5 archives with encrypted filenames can appear empty to libarchive.
    // Check the native archive flag before attempting passwords, so ordinary
    // empty archives retain their original listing semantics.
    if (!RarNativeHasEncryptedHeaders(archivePath))
        return listed;

    std::vector<ArchiveEntry> nativeEntries;
    std::string nativeError;
    if (RarNativeListHeaderEncryptedEntries(archivePath, nativeEntries, nativeError))
    {
        entries = std::move(nativeEntries);
        error.clear();
        return true;
    }

    error = "RAR filenames are encrypted; " + nativeError;
    return false;
}

bool RarExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error)
{
    // The shared memory route must also work with encrypted RAR5 entries that
    // libarchive cannot decode. Native UnRAR captures RAR_TEST output in RAM.
    ArchiveEntry nativeEntry;
    std::string lookupError;
    if (RarNativeLookupEntryMetadata(archivePath, index, nativeEntry, lookupError) && nativeEntry.isEncrypted)
    {
        // Do not allow the native header to request an unbounded allocation,
        // even if an adapter trusted a smaller size from another RAR reader.
        const size_t limit = static_cast<size_t>((std::min)(nativeEntry.size, ARCHIVE_MAX_EXTRACT_TO_MEMORY_BYTES));
        std::string nativeError;
        if (RarNativeExtractEncryptedEntryToMemory(archivePath, index, limit, data, nativeError))
        {
            error.clear();
            return true;
        }

        // Preserve the libarchive path when the native saved/runtime/interactive
        // credential candidates cannot decode the selected encrypted entry.
        if (LibarchiveRarExtractEntryToMemory(archivePath, index, data, error))
            return true;

        error = nativeError + "; " + error;
        return false;
    }

    return LibarchiveRarExtractEntryToMemory(archivePath, index, data, error);
}

bool RarExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error)
{
    if (LibarchiveRarExtractEntryToFile(archivePath, index, outputPath, error))
        return true;

    // libarchive cannot decode some encrypted RAR5 entries. Only for such an
    // extraction failure, try the existing native UnRAR static SDK with saved
    // password candidates; the normal streaming reader remains unchanged.
    const std::string libarchiveError = error;
    std::string nativeError;
    if (RarNativeExtractEncryptedEntryToFile(archivePath, index, outputPath, nativeError))
    {
        error.clear();
        return true;
    }

    error = libarchiveError + "; " + nativeError;
    return false;
}

bool RarOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error, const ArchiveStreamCancellationToken* cancellation)
{
    void* impl = nullptr;
    uint64_t entrySize = 0;

    if (!LibarchiveRarOpenEntryStream(archivePath, index, &impl, &entrySize, error, cancellation))
    {
        return false;
    }

    stream.impl = impl;
    stream.size = entrySize;
    stream.position = 0;
    stream.format = ArchiveFormat::Rar;

    return true;
}

size_t RarReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead, std::string* outError)
{
    if (outError)
        outError->clear();

    if (!stream.impl || !buffer || bytesToRead == 0)
    {
        if (outError && bytesToRead > 0)
            *outError = "Invalid RAR read request";

        return 0;
    }

    std::string error;
    size_t got = LibarchiveRarReadEntryStream(stream.impl, buffer, bytesToRead, error);

    if (!error.empty())
    {
        if (outError)
            *outError = error;

        DebugLogWrite("ERROR", "RAR", "STREAM_READ_FAIL", "requested=%zu | position=%llu | error=%s", bytesToRead, (unsigned long long)stream.position, error.c_str());
    }

    stream.position += got;
    return got;
}

bool RarSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition)
{
    if (!stream.impl)
    {
        return false;
    }

    std::string error;

    if (!LibarchiveRarSeekEntryStream(stream.impl, absolutePosition, error))
    {
        DebugLogWrite("ERROR", "RAR", "STREAM_SEEK_FAIL", "target=%llu | error=%s", (unsigned long long)absolutePosition, error.c_str());
        return false;
    }

    stream.position = absolutePosition;
    return true;
}

void RarCloseEntryStream(ArchiveBackendStream& stream)
{
    if (stream.impl)
    {
        LibarchiveRarCloseEntryStream(stream.impl);
        stream.impl = nullptr;
    }

    stream.size = 0;
    stream.position = 0;
    stream.format = ArchiveFormat::Unknown;
}