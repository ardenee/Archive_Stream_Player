// media_player_2022/cfapi_probe.cpp
// Disposable CfAPI read-only placeholder provider. Feasibility only.
#include <windows.h>
#include <cfapi.h>
#include "../archive_backend/archive_backend.h"
#include <string>
#include <mutex>
#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <vector>
#include <string_view>

namespace
{
std::wstring g_source;
std::wstring g_log;
uint64_t g_sourceBytes = 0;
bool g_fromArchive = false;
uint32_t g_entryIndex = 0;
CF_HYDRATION_POLICY_PRIMARY_USHORT g_hydrationPolicy = CF_HYDRATION_POLICY_PROGRESSIVE;
ArchiveBackendStream g_archiveStream{};
std::mutex g_streamMutex;
std::mutex g_logMutex;

void Log(const wchar_t* message, uint64_t position = 0, uint64_t length = 0, HRESULT hr = S_OK)
{
    std::lock_guard<std::mutex> lock(g_logMutex);
    FILE* f = nullptr;
    if (_wfopen_s(&f, g_log.c_str(), L"a") != 0 || !f)
        return;
    fwprintf(f, L"tick_ms=%llu provider_pid=%lu event=%ls offset=%llu length=%llu hr=0x%08X\n",
        static_cast<unsigned long long>(GetTickCount64()), GetCurrentProcessId(), message, position, length, static_cast<unsigned>(hr));
    fclose(f);
}

HRESULT SendChunk(const CF_CALLBACK_INFO* info, uint64_t offset, const void* buffer, DWORD length, NTSTATUS status)
{
    CF_OPERATION_INFO op{};
    op.StructSize = sizeof(op);
    op.Type = CF_OPERATION_TYPE_TRANSFER_DATA;
    op.ConnectionKey = info->ConnectionKey;
    op.TransferKey = info->TransferKey;
    op.CorrelationVector = info->CorrelationVector;
    op.RequestKey = info->RequestKey;
    CF_OPERATION_PARAMETERS params{};
    params.ParamSize = sizeof(params);
    params.TransferData.Flags = CF_OPERATION_TRANSFER_DATA_FLAG_NONE;
    params.TransferData.CompletionStatus = status;
    params.TransferData.Buffer = buffer;
    params.TransferData.Offset.QuadPart = static_cast<LONGLONG>(offset);
    params.TransferData.Length.QuadPart = length;
    return CfExecute(&op, &params);
}

void CALLBACK FetchData(const CF_CALLBACK_INFO* info, const CF_CALLBACK_PARAMETERS* args)
{
    const uint64_t offset = args->FetchData.RequiredFileOffset.QuadPart;
    const uint64_t length = args->FetchData.RequiredLength.QuadPart;
    Log(L"FETCH_DATA", offset, length);
    if (info->ProcessInfo)
    {
        std::lock_guard<std::mutex> logGuard(g_logMutex);
        FILE* eventLog = nullptr;
        if (_wfopen_s(&eventLog, g_log.c_str(), L"a") == 0 && eventLog)
        {
            fwprintf(eventLog, L"tick_ms=%llu event=FETCH_REQUESTER process_pid=%lu process_path=%ls required_offset=%llu required_length=%llu optional_offset=%lld optional_length=%lld flags=%u\n",
                static_cast<unsigned long long>(GetTickCount64()), info->ProcessInfo->ProcessId,
                info->ProcessInfo->ImagePath ? info->ProcessInfo->ImagePath : L"(null)",
                offset, length, args->FetchData.OptionalFileOffset.QuadPart, args->FetchData.OptionalLength.QuadPart,
                static_cast<unsigned>(args->FetchData.Flags));
            fclose(eventLog);
        }
    }
    if (length == 0 || offset >= g_sourceBytes || length > g_sourceBytes - offset)
    {
        Log(L"FETCH_RANGE_REJECTED", offset, length);
        SendChunk(info, offset, nullptr, 0, static_cast<NTSTATUS>(0xC000000D));
        return;
    }
    // Archive streams are not reentrant: serialize seeks and reads for this
    // one-track diagnostic provider. Other callers can still request ranges.
    std::unique_lock<std::mutex> archiveLock(g_streamMutex, std::defer_lock);
    if (g_fromArchive)
        archiveLock.lock();
    HANDLE source = INVALID_HANDLE_VALUE;
    if (!g_fromArchive)
    {
        source = CreateFileW(g_source.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (source == INVALID_HANDLE_VALUE)
        {
            Log(L"FETCH_SOURCE_UNAVAILABLE", offset, length, HRESULT_FROM_WIN32(GetLastError()));
            SendChunk(info, offset, nullptr, 0, static_cast<NTSTATUS>(0xC000000F));
            return;
        }
    }
    const size_t capacity = 64u * 1024u;
    BYTE* buffer = static_cast<BYTE*>(VirtualAlloc(nullptr, capacity, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!buffer)
    {
        if (source != INVALID_HANDLE_VALUE)
            CloseHandle(source);
        SendChunk(info, offset, nullptr, 0, static_cast<NTSTATUS>(0xC0000017));
        return;
    }
    uint64_t done = 0;
    while (done < length)
    {
        const uint64_t current = offset + done;
        const DWORD chunk = static_cast<DWORD>(std::min<uint64_t>(capacity, length - done));
        DWORD got = 0;
        bool readOk = false;
        if (g_fromArchive)
        {
            // Supply uncompressed MP3 bytes directly from archive_backend.
            // This uses header sizes only, never a calculated content CRC.
            if (ArchiveSeekEntryStream(g_archiveStream, current))
            {
                size_t total = 0;
                while (total < chunk)
                {
                    const size_t read = ArchiveReadEntryStream(g_archiveStream, buffer + total, chunk - total);
                    if (read == 0)
                        break;
                    total += read;
                }
                got = static_cast<DWORD>(total);
                readOk = got == chunk;
            }
        }
        else
        {
            LARGE_INTEGER seek{};
            seek.QuadPart = static_cast<LONGLONG>(current);
            readOk = SetFilePointerEx(source, seek, nullptr, FILE_BEGIN) &&
                ReadFile(source, buffer, chunk, &got, nullptr) && got == chunk;
        }
        if (!readOk)
        {
            Log(g_fromArchive ? L"FETCH_ARCHIVE_READ_FAILED" : L"FETCH_SOURCE_READ_FAILED", current, chunk, g_fromArchive ? E_FAIL : HRESULT_FROM_WIN32(GetLastError()));
            SendChunk(info, current, nullptr, 0, static_cast<NTSTATUS>(0xC0000001));
            break;
        }
        const HRESULT hr = SendChunk(info, current, buffer, got, 0);
        Log(L"TRANSFER_DATA", current, got, hr);
        if (FAILED(hr))
            break;
        done += got;
    }
    VirtualFree(buffer, 0, MEM_RELEASE);
    if (source != INVALID_HANDLE_VALUE)
        CloseHandle(source);
    Log(L"FETCH_COMPLETED", offset, done);
}

void SnapshotPlaceholder(const std::wstring& root, const std::wstring& name, const wchar_t* event)
{
    const std::wstring file = root + L"\\" + name;
    HANDLE handle = CreateFileW(file.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
    {
        Log(L"SNAPSHOT_OPEN_FAILED", 0, 0, HRESULT_FROM_WIN32(GetLastError()));
        return;
    }
    FILE_STANDARD_INFO standard{};
    if (GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)))
        Log(event, static_cast<uint64_t>(standard.AllocationSize.QuadPart), static_cast<uint64_t>(standard.EndOfFile.QuadPart));
    else
        Log(L"SNAPSHOT_FILEINFO_FAILED", 0, 0, HRESULT_FROM_WIN32(GetLastError()));
    alignas(CF_PLACEHOLDER_STANDARD_INFO) BYTE infoBuffer[512]{};
    DWORD returned = 0;
    const HRESULT hr = CfGetPlaceholderInfo(handle, CF_PLACEHOLDER_INFO_STANDARD, infoBuffer, sizeof(infoBuffer), &returned);
    if (SUCCEEDED(hr))
    {
        const auto* info = reinterpret_cast<const CF_PLACEHOLDER_STANDARD_INFO*>(infoBuffer);
        Log(L"CF_ONDISK_BYTES", static_cast<uint64_t>(info->OnDiskDataSize.QuadPart), static_cast<uint64_t>(info->ValidatedDataSize.QuadPart));
    }
    else
    {
        Log(L"CF_INFO_FAILED", 0, 0, hr);
    }
    CloseHandle(handle);
}

void Cleanup(const std::wstring& root, const std::wstring& filename)
{
    const HRESULT hr = CfUnregisterSyncRoot(root.c_str());
    Log(L"UNREGISTER_ROOT", 0, 0, hr);
    if (!filename.empty())
    {
        const std::wstring file = root + L"\\" + filename;
        // QA-only root: after unregistration the placeholder may become an
        // ordinary read-only file. Clear that flag before deleting it.
        const DWORD attributes = GetFileAttributesW(file.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY))
            SetFileAttributesW(file.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(file.c_str()))
            Log(L"REMOVE_PLACEHOLDER");
        else if (GetLastError() != ERROR_FILE_NOT_FOUND)
            Log(L"REMOVE_PLACEHOLDER_FAILED", 0, 0, HRESULT_FROM_WIN32(GetLastError()));
    }
    if (RemoveDirectoryW(root.c_str()))
        Log(L"REMOVE_ROOT");
    else if (GetLastError() != ERROR_PATH_NOT_FOUND)
        Log(L"REMOVE_ROOT_FAILED", 0, 0, HRESULT_FROM_WIN32(GetLastError()));
}
}

int wmain(int argc, wchar_t** argv)
{
    // Supported default: run or run-archive (PROGRESSIVE + STREAMING_ALLOWED).
    // Explicit unsupported-policy regression: run-partial or run-archive-partial.
    // Disk-hydration control: run-progressive-disk.
    // Safe cleanup: cfapi_probe.exe cleanup ROOT FILENAME LOG
    if (argc >= 5 && wcscmp(argv[1], L"cleanup") == 0)
    {
        g_log = argv[4];
        Cleanup(argv[2], argv[3]);
        return 0;
    }
    if (argc < 7)
        return 1;
    const std::wstring_view mode(argv[1]);
    if (mode != L"run" && mode != L"run-progressive" && mode != L"run-progressive-disk" && mode != L"run-partial" &&
        mode != L"run-archive" && mode != L"run-archive-progressive" && mode != L"run-archive-partial")
        return 1;
    g_fromArchive = mode.find(L"archive") != std::wstring_view::npos;
    if (mode.find(L"partial") != std::wstring_view::npos)
        g_hydrationPolicy = CF_HYDRATION_POLICY_PARTIAL;

    const std::wstring root = argv[2];
    g_source = argv[3];
    const std::wstring ready = argv[4];
    const std::wstring stop = argv[5];
    g_log = argv[6];
    const size_t separator = g_source.find_last_of(L"\\/");
    std::wstring name = separator == std::wstring::npos ? g_source : g_source.substr(separator + 1);
    if (g_fromArchive)
        name = L"archive-track-0.mp3";
    HANDLE source = CreateFileW(g_source.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (source == INVALID_HANDLE_VALUE)
        return 2;
    LARGE_INTEGER fileSize{};
    FILE_BASIC_INFO basic{};
    if (!GetFileSizeEx(source, &fileSize) ||
        !GetFileInformationByHandleEx(source, FileBasicInfo, &basic, sizeof(basic)))
    {
        CloseHandle(source);
        return 3;
    }
    CloseHandle(source);
    if (g_fromArchive)
    {
        // Select one directly streamable MP3 for a bounded proof of concept.
        // No entry bytes are materialized, and size comes from archive headers.
        std::vector<ArchiveEntry> entries;
        std::string error;
        if (!ArchiveListEntries(g_source.c_str(), entries, error))
        {
            Log(L"ARCHIVE_LIST_FAILED");
            return 10;
        }
        const auto found = std::find_if(entries.begin(), entries.end(), [](const ArchiveEntry& entry)
        {
            if (entry.isFolder || !entry.isSupportedAudio || entry.isSolid || entry.isEncrypted || entry.size < 1024)
                return false;
            const size_t dot = entry.path.find_last_of('.');
            return dot != std::string::npos && _stricmp(entry.path.c_str() + dot, ".mp3") == 0;
        });
        if (found == entries.end())
        {
            Log(L"ARCHIVE_MP3_STREAMABLE_NOT_FOUND");
            return 11;
        }
        g_entryIndex = found->index;
        if (!ArchiveOpenEntryStream(g_source.c_str(), g_entryIndex, g_archiveStream, error))
        {
            Log(L"ARCHIVE_STREAM_OPEN_FAILED", g_entryIndex);
            return 12;
        }
        g_sourceBytes = found->size;
        fileSize.QuadPart = static_cast<LONGLONG>(found->size);
        Log(L"ARCHIVE_STREAM_OPEN", g_entryIndex, g_sourceBytes);
    }
    else
    {
        g_sourceBytes = static_cast<uint64_t>(fileSize.QuadPart);
    }

    if (!CreateDirectoryW(root.c_str(), nullptr))
    {
        const DWORD error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS)
            return 4;
        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &data);
        if (find != INVALID_HANDLE_VALUE)
        {
            do
            {
                if (wcscmp(data.cFileName, L".") && wcscmp(data.cFileName, L".."))
                {
                    FindClose(find);
                    Log(L"ROOT_NOT_EMPTY");
                    return 5;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }

    const wchar_t identity[] = L"ArchiveStreamCFProbeV1";
    CF_SYNC_REGISTRATION registration{};
    registration.StructSize = sizeof(registration);
    registration.ProviderName = L"Archive Stream Player QA (temporary)";
    registration.ProviderVersion = L"0.0.1";
    registration.ProviderId = {0xB42F0659,0xFE28,0x430A,{0x98,0x13,0x1D,0x18,0x30,0x5C,0x2D,0xFE}};
    registration.SyncRootIdentity = identity;
    registration.SyncRootIdentityLength = sizeof(identity);
    registration.FileIdentity = identity;
    registration.FileIdentityLength = sizeof(identity);
    CF_SYNC_POLICIES policies{};
    policies.StructSize = sizeof(policies);
    policies.Hydration.Primary = g_hydrationPolicy;
    Log(g_hydrationPolicy == CF_HYDRATION_POLICY_PROGRESSIVE ? L"POLICY_PROGRESSIVE" : L"POLICY_PARTIAL_UNSUPPORTED");
    policies.Hydration.Modifier = mode == L"run-progressive-disk" ? CF_HYDRATION_POLICY_MODIFIER_NONE : CF_HYDRATION_POLICY_MODIFIER_STREAMING_ALLOWED;
    Log(policies.Hydration.Modifier == CF_HYDRATION_POLICY_MODIFIER_NONE ? L"MODIFIER_DISK_REQUIRED" : L"MODIFIER_STREAMING_ALLOWED");
    policies.Population.Primary = CF_POPULATION_POLICY_ALWAYS_FULL;
    policies.HardLink = CF_HARDLINK_POLICY_NONE;
    policies.PlaceholderManagement = CF_PLACEHOLDER_MANAGEMENT_POLICY_DEFAULT;
    HRESULT hr = CfRegisterSyncRoot(root.c_str(), &registration, &policies, CF_REGISTER_FLAG_MARK_IN_SYNC_ON_ROOT);
    Log(L"REGISTER_ROOT", 0, 0, hr);
    if (FAILED(hr))
        return 6;
    const CF_CALLBACK_REGISTRATION callbacks[] =
    {
        { CF_CALLBACK_TYPE_FETCH_DATA, FetchData },
        CF_CALLBACK_REGISTRATION_END
    };
    CF_CONNECTION_KEY key{};
    hr = CfConnectSyncRoot(root.c_str(), callbacks, nullptr,
        static_cast<CF_CONNECT_FLAGS>(CF_CONNECT_FLAG_REQUIRE_PROCESS_INFO | CF_CONNECT_FLAG_REQUIRE_FULL_FILE_PATH), &key);
    Log(L"CONNECT_ROOT", 0, 0, hr);
    if (FAILED(hr))
    {
        Cleanup(root, name);
        return 7;
    }
    CF_PLACEHOLDER_CREATE_INFO placeholder{};
    placeholder.RelativeFileName = name.c_str();
    placeholder.FsMetadata.BasicInfo = basic;
    placeholder.FsMetadata.BasicInfo.FileAttributes |= FILE_ATTRIBUTE_READONLY;
    placeholder.FsMetadata.FileSize = fileSize;
    placeholder.FileIdentity = identity;
    placeholder.FileIdentityLength = sizeof(identity);
    placeholder.Flags = CF_PLACEHOLDER_CREATE_FLAG_MARK_IN_SYNC;
    DWORD processed = 0;
    hr = CfCreatePlaceholders(root.c_str(), &placeholder, 1, CF_CREATE_FLAG_STOP_ON_ERROR, &processed);
    Log(L"CREATE_PLACEHOLDER", 0, g_sourceBytes, hr);
    if (SUCCEEDED(hr))
    {
        SnapshotPlaceholder(root, name, L"SNAPSHOT_INITIAL_ALLOCATION");
        HANDLE status = CreateFileW(ready.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (status != INVALID_HANDLE_VALUE)
            CloseHandle(status);
        // Bound the diagnostic provider lifetime even if the controlling test dies.
        for (unsigned i = 0; i < 300; ++i)
        {
            if (GetFileAttributesW(stop.c_str()) != INVALID_FILE_ATTRIBUTES)
                break;
            Sleep(200);
        }
    }
    if (SUCCEEDED(hr))
        SnapshotPlaceholder(root, name, L"SNAPSHOT_FINAL_ALLOCATION");
    CfDisconnectSyncRoot(key);
    if (g_fromArchive)
    {
        ArchiveCloseEntryStream(g_archiveStream);
        Log(L"ARCHIVE_STREAM_CLOSED", g_entryIndex);
    }
    Cleanup(root, name);
    return FAILED(hr) ? 8 : 0;
}
