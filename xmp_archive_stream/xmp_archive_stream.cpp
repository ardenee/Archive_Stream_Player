// XMPlay native archive adapter (XMPARC v4.1).
// XMPlay requests an entire decompressed entry as a memory block. The backend
// prefers incremental decoding when possible; this interface cannot hand off
// a live entry stream to XMPlay's decoder.
#define NOMINMAX
#include <windows.h>
#include <wincred.h>
#include <xmparc.h>
#include "archive_backend.h"
#include "archive_adapter_preferences.h"
#include "../shared/archive_adapter_logging.h"
#include "../shared/archive_unified_cache.h"
#include "../shared/archive_unified_warmer.h"
#include "archive_common.h"
#include "archive_format.h"
#include "archive_passwords.h"
#include "debug_log.h"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <set>
#include <string>
#include <vector>

namespace
{
    XMPFUNC_FILE* g_files = nullptr;
    XMPFUNC_MISC* g_misc = nullptr;
    HMODULE g_module = nullptr;

    std::wstring ToWide(const char* utf8)
    {
        if (!utf8 || !*utf8)
            return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, nullptr, 0);
        if (count <= 0)
            return {};
        std::wstring result(static_cast<size_t>(count), L'\0');
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, result.data(), count) != count)
            return {};
        result.pop_back();
        return result;
    }

    bool LocalArchivePath(XMPFILE file, std::wstring& path)
    {
        path.clear();
        if (!g_files || !file || g_files->GetType(file) != XMPFILE_TYPE_FILE)
            return false;
        path = ToWide(g_files->GetFilename(file));
        return !path.empty() && DetectArchiveFormatFromPath(path.c_str()) != ArchiveFormat::Unknown;
    }

    BOOL WINAPI CheckFile(XMPFILE file)
    {
        try
        {
            std::wstring path;
            if (!LocalArchivePath(file, path) || !g_files->Seek64 || !g_files->Tell64)
                return FALSE;
            const QWORD position = g_files->Tell64(file);
            unsigned char header[8]{};
            if (!g_files->Seek64(file, 0))
                return FALSE;
            const DWORD size = g_files->Read(file, header, sizeof(header));
            g_files->Seek64(file, position);
            switch (DetectArchiveFormatFromPath(path.c_str()))
            {
            case ArchiveFormat::SevenZip:
                return size >= 6 && memcmp(header, "\x37\x7A\xBC\xAF\x27\x1C", 6) == 0;
            case ArchiveFormat::Rar:
                return size >= 7 && memcmp(header, "Rar!\x1A\x07", 6) == 0;
            case ArchiveFormat::Zip:
                return size >= 4 && memcmp(header, "PK\x03\x04", 4) == 0;
            default:
                return FALSE;
            }
        }
        catch (...)
        {
            return FALSE;
        }
    }

    // Duplicate paths must roundtrip unambiguously through the XMPARC entry
    // string. Only collisions receive an index-containing parent directory.
    std::string EntryName(const ArchiveEntry& entry, const std::set<std::string>& duplicates)
    {
        return duplicates.find(entry.path) == duplicates.end()
            ? entry.path : std::to_string(entry.index) + "/" + entry.path;
    }

    bool Collect(const std::wstring& path, std::vector<ArchiveEntry>& entries, std::set<std::string>& duplicates)
    {
        std::string error;
        if (!ArchiveListEntries(path.c_str(), entries, error))
        {
            DebugLogWrite("WARN", "XMPLAY", "LIST_FAIL", "error=%s", error.c_str());
            return false;
        }
        std::set<std::string> seen;
        for (const auto& entry : entries)
        {
            if (!entry.isFolder && entry.isSupportedAudio && !seen.insert(entry.path).second)
                duplicates.insert(entry.path);
        }
        return true;
    }

    char* WINAPI GetFileList(XMPFILE file)
    {
        try
        {
            std::wstring path;
            if (!LocalArchivePath(file, path))
                return nullptr;
            std::vector<ArchiveEntry> entries;
            std::set<std::string> duplicates;
            if (!Collect(path, entries, duplicates))
                return nullptr;
            size_t bytes = 1;
            unsigned count = 0;
            for (const auto& entry : entries)
            {
                if (entry.isFolder || !entry.isSupportedAudio)
                    continue;
                const std::string name = EntryName(entry, duplicates);
                if (name.empty() || name.size() > 32767 || bytes > UINT32_MAX - name.size() - 1)
                    return nullptr;
                bytes += name.size() + 1;
                ++count;
            }
            if (!count || bytes > UINT32_MAX)
                return nullptr;
            char* output = static_cast<char*>(g_misc->Alloc(static_cast<DWORD>(bytes)));
            if (!output)
                return nullptr;
            size_t offset = 0;
            for (const auto& entry : entries)
            {
                if (entry.isFolder || !entry.isSupportedAudio)
                    continue;
                const std::string name = EntryName(entry, duplicates);
                memcpy(output + offset, name.c_str(), name.size() + 1);
                offset += name.size() + 1;
            }
            output[offset] = '\0';
            DebugLogWrite("INFO", "XMPLAY", "LIST_OK", "count=%u", count);
            return output;
        }
        catch (...)
        {
            return nullptr;
        }
    }

    bool AskPassword(const char* archive, const char*, char* output, size_t outputSize, bool* save)
    {
        if (!output || !outputSize)
            return FALSE;
        output[0] = '\0';
        const auto preferences = ArchiveAdapterPreferencesLoad();
        if (!preferences.promptPasswords)
            return false;
        if (save)
            *save = false;
        const std::wstring target = ToWide(archive);
        CREDUI_INFOW ui{};
        ui.cbSize = sizeof(ui);
        ui.hwndParent = g_misc ? g_misc->GetWindow() : nullptr;
        ui.pszCaptionText = L"Archive Stream Password";
        ui.pszMessageText = L"Enter the archive password. Username is unused; credentials are not stored by Windows.";
        wchar_t username[CREDUI_MAX_USERNAME_LENGTH + 1]{};
        wchar_t password[CREDUI_MAX_PASSWORD_LENGTH + 1]{};
        BOOL persist = FALSE;
        const DWORD flags = CREDUI_FLAGS_GENERIC_CREDENTIALS | CREDUI_FLAGS_ALWAYS_SHOW_UI | CREDUI_FLAGS_DO_NOT_PERSIST;
        const DWORD rc = CredUIPromptForCredentialsW(&ui,
            target.empty() ? L"Archive Stream" : target.c_str(), nullptr, 0,
            username, CREDUI_MAX_USERNAME_LENGTH + 1,
            password, CREDUI_MAX_PASSWORD_LENGTH + 1, &persist, flags);
        if (rc != NO_ERROR)
        {
            SecureZeroMemory(password, sizeof(password));
            return FALSE;
        }
        const int len = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1, nullptr, 0, nullptr, nullptr);
        const bool valid = len > 1 && static_cast<size_t>(len) <= outputSize &&
            WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, password, -1,
                output, static_cast<int>(outputSize), nullptr, nullptr) == len;
        SecureZeroMemory(password, sizeof(password));
        if (!valid)
            output[0] = '\0';
        if (valid && save && preferences.askToSavePassword)
            *save = MessageBoxW(g_misc ? g_misc->GetWindow() : nullptr, L"Save the password to the configured plaintext password list after verification?", L"Archive Stream - XMPlay", MB_YESNO | MB_ICONQUESTION) == IDYES;
        return valid ? TRUE : FALSE;
    }

    bool ReadCachedBytes(const std::wstring& path, unsigned char* output, DWORD wanted)
    {
        HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            return false;
        DWORD read = 0;
        const bool ok = ReadFile(handle, output, wanted, &read, nullptr) && read == wanted;
        CloseHandle(handle);
        return ok;
    }

    void* WINAPI DecompressFile(XMPFILE file, const char* name, DWORD* length)
    {
        if (!name || !length || !g_misc)
            return nullptr;
        const DWORD requested = *length;
        *length = 0;
        if (requested == 0)
            return nullptr;
        unsigned char* result = nullptr;
        try
        {
            std::wstring path;
            if (!LocalArchivePath(file, path))
                return nullptr;
            std::vector<ArchiveEntry> entries;
            std::set<std::string> duplicates;
            if (!Collect(path, entries, duplicates))
                return nullptr;
            const auto entry = std::find_if(entries.begin(), entries.end(), [&](const ArchiveEntry& e)
            {
                return !e.isFolder && e.isSupportedAudio && EntryName(e, duplicates) == name;
            });
            if (entry == entries.end() || entry->size == 0)
                return nullptr;

            // XMPARC's input length is a maximum, not necessarily the entire
            // entry. XMPlay may request just a header to probe metadata.
            const DWORD wanted = static_cast<DWORD>((std::min)(entry->size, static_cast<uint64_t>(requested)));
            result = static_cast<unsigned char*>(g_misc->Alloc(wanted));
            if (!result)
                return nullptr;

            bool extracted = false;
            std::string error;
            ArchiveBackendStream stream{};
            const ArchiveFormat format = DetectArchiveFormatFromPath(path.c_str());
            const bool solid = entry->isSolid && (format == ArchiveFormat::SevenZip || format == ArchiveFormat::Rar);
            if (!solid && ArchiveOpenEntryStream(path.c_str(), entry->index, stream, error))
            {
                DWORD transferred = 0;
                while (transferred < wanted)
                {
                    if (g_misc->CheckCancel())
                        break;
                    const size_t bytes = ArchiveReadEntryStream(stream, result + transferred, wanted - transferred);
                    if (!bytes)
                        break;
                    transferred += static_cast<DWORD>(bytes);
                }
                extracted = transferred == wanted;
                ArchiveCloseEntryStream(stream);
                if (extracted)
                    DebugLogWrite("INFO", "XMPLAY", "STREAM_OK", "index=%u | bytes=%u", entry->index, wanted);
            }

            const auto preferences = ArchiveAdapterPreferencesLoad();
            std::wstring cachePath;
            if (!extracted && !g_misc->CheckCancel() && entry->size <= static_cast<uint64_t>(preferences.memoryMb) * 1024 * 1024)
            {
                std::vector<unsigned char> decoded;
                if (ArchiveExtractEntryToMemory(path.c_str(), entry->index, decoded, error) &&
                    decoded.size() == entry->size)
                {
                    memcpy(result, decoded.data(), wanted);
                    extracted = true;
                    DebugLogWrite("INFO", "XMPLAY", "MEMORY_OK", "index=%u | bytes=%u", entry->index, wanted);
                }
            }

            if (!extracted && !g_misc->CheckCancel() &&
                ArchiveUnifiedCache::Prepare(path, *entry, preferences, cachePath, error))
            {
                extracted = ReadCachedBytes(cachePath, result, wanted);
                if (extracted)
                {
                    DebugLogWrite("INFO", "XMPLAY", "CACHE_OK", "index=%u | bytes=%u", entry->index, wanted);
                    ArchiveUnifiedWarmer::Queue(path, *entry, preferences);
                }
            }

            if (!extracted)
            {
                g_misc->Free(result);
                DebugLogWrite("WARN", "XMPLAY", "DECOMPRESS_FAIL", "index=%u | error=%s", entry->index, error.c_str());
                return nullptr;
            }
            *length = wanted;
            return result;
        }
        catch (...)
        {
            if (result)
                g_misc->Free(result);
            return nullptr;
        }
    }

    void WINAPI Configure(HWND window)
    {
        ArchiveAdapterOpenSettings(g_module, window);
    }

    void WINAPI About(HWND window)
    {
        MessageBoxW(window, L"Archive Stream Player for XMPlay.\nZIP, 7z, RAR, encryption and multipart archives.\n"
            L"Static shared backend; streaming, memory, then reusable shared cache fallback.\n"
            L"XMPlay receives the requested decoded bytes in memory; partial reads are supported.",
            L"Archive Stream Player", MB_OK | MB_ICONINFORMATION);
    }
}

extern "C" XMPARC* WINAPI XMPARC_GetInterface(DWORD face, InterfaceProc get)
{
    if (face != XMPARC_FACE || !get)
        return nullptr;
    auto* files = static_cast<XMPFUNC_FILE*>(get(XMPFUNC_FILE_FACE));
    auto* misc = static_cast<XMPFUNC_MISC*>(get(XMPFUNC_MISC_FACE));
    if (!files || !misc)
        return nullptr;
    g_files = files;
    g_misc = misc;
    ArchiveSetDataDirectoryName("XMPlayArchiveStream");
    const auto preferences = ArchiveAdapterPreferencesLoad();
    if (!preferences.passwordListPath.empty())
    {
        const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, preferences.passwordListPath.c_str(), -1, nullptr, 0, nullptr, nullptr);
        if (bytes > 1)
        {
            std::string path(static_cast<size_t>(bytes), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, preferences.passwordListPath.c_str(), -1, path.data(), bytes, nullptr, nullptr) == bytes)
                ArchivePasswordSetListPath(path.c_str());
        }
    }
    ArchivePasswordSetPromptCallback(AskPassword);
    static const bool traceConfigured = []()
    {
        char enabled[8]{};
        const bool debugOverride = GetEnvironmentVariableA("ARCHIVE_STREAM_ADAPTER_DEBUG", enabled, sizeof(enabled)) > 0 && enabled[0] == '1';
        const auto preferences = ArchiveAdapterPreferencesLoad();
        ArchiveAdapterLogging::Configure(L"xmplay", preferences.debugLogging || debugOverride);
        if (!ArchiveUnifiedCache::Maintain(preferences))
            DebugLogWrite("WARN", "XMPLAY", "CACHE_MAINTENANCE_PARTIAL", "reason=cache_file_in_use");
        return true;
    }();
    (void)traceConfigured;
    DebugLogWrite("INFO", "XMPLAY", "PLUGIN_INIT", "xmparc=1");
    static XMPARC adapter{
        XMPARC_FLAG_CONFIG,
        "Archive Stream ZIP/7z/RAR\0zip/7z/rar/001",
        CheckFile, GetFileList, DecompressFile,
        About, Configure, nullptr
    };
    return &adapter;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        g_module = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}
