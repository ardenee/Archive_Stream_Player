#pragma once
// Host-independent managed fallback cache for every Archive Stream adapter.
// Cache entries are identified by the archive's canonical path, file metadata,
// entry index, decoded length, CRC (when supplied) and original internal path.
// New files are extracted to a private sibling and published atomically.
// Other host/plugin caches and password files are never migrated or deleted.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <new>
#include <string>
#include <vector>
#include "archive_adapter_preferences.h"
#include "archive_backend.h"
#include "archive_format.h"
#include "archive_rar_native_encrypted.h"
#include "archive_7z_reader.h"
#include "archive_passwords.h"
#include "debug_log.h"

namespace ArchiveUnifiedCache
{
    constexpr uint64_t kMb = 1024ull * 1024ull;
    struct FileInfo
    {
        std::wstring path;
        uint64_t size = 0;
        FILETIME modified{};
    };

    inline uint64_t HashBytes(uint64_t hash, const void* ptr, size_t bytes)
    {
        const auto* input = static_cast<const unsigned char*>(ptr);
        for (size_t i = 0; i < bytes; ++i)
        {
            hash ^= input[i];
            hash *= 1099511628211ull;
        }
        return hash;
    }

    template <typename T>
    inline void HashValue(uint64_t& hash, const T& value)
    {
        hash = HashBytes(hash, &value, sizeof(value));
    }

    inline void HashWide(uint64_t& hash, const std::wstring& value)
    {
        for (const wchar_t ch : value)
        {
            const wchar_t folded = static_cast<wchar_t>(towlower(ch));
            HashValue(hash, folded);
        }
    }

    inline bool ArchiveIdentity(const std::wstring& archive, uint64_t& hash, std::string& error)
    {
        wchar_t absolute[MAX_PATH * 4]{};
        const DWORD written = GetFullPathNameW(archive.c_str(), _countof(absolute), absolute, nullptr);
        if (written == 0 || written >= _countof(absolute))
        {
            error = "Unable to canonicalize archive path";
            return false;
        }
        HashWide(hash, absolute);
        std::wstring volume = absolute;
        const auto dot = volume.find_last_of(L'.');
        const bool multipart = dot != std::wstring::npos &&
            volume.size() - dot == 4 && volume.compare(dot, 4, L".001") == 0;
        unsigned volumes = 0;
        for (;;)
        {
            WIN32_FILE_ATTRIBUTE_DATA attrs{};
            if (!GetFileAttributesExW(volume.c_str(), GetFileExInfoStandard, &attrs) ||
                (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                if (!volumes)
                {
                    error = "Archive path not found while checking cache identity";
                    return false;
                }
                break;
            }
            ++volumes;
            HashValue(hash, attrs.nFileSizeLow);
            HashValue(hash, attrs.nFileSizeHigh);
            HashValue(hash, attrs.ftLastWriteTime.dwLowDateTime);
            HashValue(hash, attrs.ftLastWriteTime.dwHighDateTime);

            // A later 7z/ZIP volume changing must invalidate any shared cache
            // even when the first volume and its modification time are intact.
            if (!multipart || volumes >= 999)
                break;
            wchar_t suffix[5]{};
            swprintf_s(suffix, L".%03u", volumes + 1);
            volume.replace(dot, 4, suffix);
            // Advance through a complete contiguous series.
            if (volume.size() > 4095)
                break;
        }
        HashValue(hash, volumes);
        return true;
    }

    inline std::wstring Folder()
    {
        wchar_t local[MAX_PATH * 4]{};
        const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, _countof(local));
        if (!n || n >= _countof(local))
            return {};
        const std::wstring root = std::wstring(local) + L"\\ArchiveStreamPlayer";
        if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
        const std::wstring folder = root + L"\\ManagedCache";
        if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
            return {};
        return folder;
    }

    inline bool Path(const std::wstring& archive, const ArchiveEntry& entry,
        std::wstring& result, std::string& error)
    {
        result.clear();
        if (archive.empty() || entry.size == 0 || entry.isFolder)
        {
            error = "Invalid archive entry for managed cache";
            return false;
        }
        const std::wstring folder = Folder();
        if (folder.empty())
        {
            error = "Shared managed-cache directory is unavailable";
            return false;
        }

        uint64_t hash = 14695981039346656037ull;
        if (!ArchiveIdentity(archive, hash, error))
            return false;
        HashValue(hash, entry.index);
        HashValue(hash, entry.size);
        HashValue(hash, entry.crc32);
        HashValue(hash, entry.hasCrc32);
        hash = HashBytes(hash, entry.path.data(), entry.path.size());

        std::wstring extension = L".bin";
        const auto dot = entry.path.find_last_of('.');
        const auto slash = entry.path.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash) &&
            entry.path.size() - dot <= 12)
        {
            std::wstring candidate;
            for (size_t i = dot; i < entry.path.size(); ++i)
            {
                const unsigned char c = static_cast<unsigned char>(entry.path[i]);
                if (c == '.' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9'))
                    candidate += static_cast<wchar_t>(towlower(c));
                else
                {
                    candidate.clear();
                    break;
                }
            }
            if (!candidate.empty())
                extension = candidate;
        }

        wchar_t name[96]{};
        swprintf_s(name, L"%016llX_%llu", static_cast<unsigned long long>(hash),
            static_cast<unsigned long long>(entry.size));
        result = folder + L"\\" + name + extension;
        return true;
    }

    inline bool LookupEntry(const std::wstring& archive, uint32_t index, ArchiveEntry& entry, std::string& error)
    {
        std::vector<ArchiveEntry> entries;
        if (!ArchiveListEntries(archive.c_str(), entries, error))
            return false;
        const auto found = std::find_if(entries.begin(), entries.end(), [index](const ArchiveEntry& item)
        {
            return item.index == index && !item.isFolder && item.isSupportedAudio;
        });
        if (found == entries.end())
        {
            error = "The requested audio entry is missing from the archive";
            return false;
        }
        entry = *found;
        return true;
    }

    inline bool ValidateFile(const std::wstring& path, uint64_t length)
    {
        WIN32_FILE_ATTRIBUTE_DATA attrs{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attrs) ||
            (attrs.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            return false;
        const uint64_t bytes = (static_cast<uint64_t>(attrs.nFileSizeHigh) << 32) | attrs.nFileSizeLow;
        return bytes == length;
    }

    inline void Touch(const std::wstring& path)
    {
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            FILETIME now{};
            GetSystemTimeAsFileTime(&now);
            SetFileTime(file, nullptr, &now, &now);
            CloseHandle(file);
        }
    }

    inline bool Find(const std::wstring& archive, const ArchiveEntry& entry,
        const ArchiveAdapterPreferences& settings, std::wstring& result)
    {
        result.clear();
        if (settings.cacheLimitMb == 0 || entry.size > static_cast<uint64_t>(settings.cacheLimitMb) * kMb)
            return false;

        std::string error;
        if (!Path(archive, entry, result, error) || !ValidateFile(result, entry.size))
        {
            result.clear();
            return false;
        }
        Touch(result);
        return true;
    }

    inline bool IsManagedName(const std::wstring& name)
    {
        if (name.size() < 22 || name[16] != L'_')
            return false;
        for (size_t i = 0; i < 16; ++i)
        {
            const wchar_t c = name[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F')))
                return false;
        }
        size_t position = 17;
        while (position < name.size() && name[position] >= L'0' && name[position] <= L'9')
            ++position;
        if (position == 17 || position >= name.size() || name[position] != L'.' ||
            name.size() - position > 11)
            return false;
        for (size_t i = position + 1; i < name.size(); ++i)
        {
            const wchar_t c = name[i];
            if (!((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z')))
                return false;
        }
        return position + 1 < name.size();
    }

    inline bool Cleanup(uint64_t allowedBytes, const std::wstring& keep = {}, unsigned maxAgeDays = 0)
    {
        const std::wstring folder = Folder();
        if (folder.empty())
            return false;

        WIN32_FIND_DATAW found{};
        const std::wstring pattern = folder + L"\\*";
        HANDLE search = FindFirstFileW(pattern.c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
            return true;

        std::vector<FileInfo> files;
        uint64_t total = 0;
        do
        {
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                continue;
            const std::wstring name = found.cFileName;
            if (!IsManagedName(name))
                continue;
            FileInfo item;
            item.path = folder + L"\\" + name;
            item.size = (static_cast<uint64_t>(found.nFileSizeHigh) << 32) | found.nFileSizeLow;
            item.modified = found.ftLastWriteTime;
            if (UINT64_MAX - total < item.size)
            {
                FindClose(search);
                return false;
            }
            total += item.size;
            files.push_back(std::move(item));
        } while (FindNextFileW(search, &found));
        FindClose(search);

        std::sort(files.begin(), files.end(), [](const FileInfo& a, const FileInfo& b)
        {
            const LONG order = CompareFileTime(&a.modified, &b.modified);
            return order < 0;
        });

        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const uint64_t nowValue = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        const uint64_t maxAge = static_cast<uint64_t>(maxAgeDays) * 24 * 60 * 60 * 10000000ull;
        for (const auto& file : files)
        {
            if (_wcsicmp(file.path.c_str(), keep.c_str()) == 0)
                continue;
            const uint64_t when = (static_cast<uint64_t>(file.modified.dwHighDateTime) << 32) | file.modified.dwLowDateTime;
            const bool expired = maxAgeDays != 0 && nowValue > when && nowValue - when > maxAge;
            if (!expired && total <= allowedBytes)
                continue;
            if (DeleteFileW(file.path.c_str()))
                total -= file.size;
        }
        return total <= allowedBytes;
    }

    inline bool Prepare(const std::wstring& archive, const ArchiveEntry& entry,
        const ArchiveAdapterPreferences& settings, std::wstring& result, std::string& error)
    {
        if (Find(archive, entry, settings, result))
            return true;
        result.clear();
        if (settings.cacheLimitMb == 0 || entry.size == 0 ||
            entry.size > static_cast<uint64_t>(settings.cacheLimitMb) * kMb)
        {
            error = "Managed cache is disabled or the entry exceeds its budget";
            return false;
        }
        if (!Path(archive, entry, result, error))
            return false;

        // Cross-process mutex protects cache publication and eviction from
        // duplicate plugin instances/HTTP bridge sessions.
        uint64_t lockKey = 14695981039346656037ull;
        HashWide(lockKey, result);
        wchar_t lockName[100]{};
        swprintf_s(lockName, L"Local\\ArchiveStreamPlayer-Cache-%016llX",
            static_cast<unsigned long long>(lockKey));
        HANDLE mutex = CreateMutexW(nullptr, FALSE, lockName);
        if (!mutex)
        {
            error = "Unable to lock managed cache";
            return false;
        }
        const DWORD wait = WaitForSingleObject(mutex, 300000);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED)
        {
            CloseHandle(mutex);
            error = "Timed out acquiring managed-cache lock";
            return false;
        }

        bool ok = false;
        do
        {
            if (ValidateFile(result, entry.size))
            {
                Touch(result);
                ok = true;
                break;
            }

            const uint64_t limit = static_cast<uint64_t>(settings.cacheLimitMb) * kMb;
            if (settings.cacheAutoCleanup)
                Cleanup(limit - entry.size, result, settings.cacheAgeDays);

            wchar_t suffix[80]{};
            swprintf_s(suffix, L".partial-%lu-%lu", GetCurrentProcessId(), GetCurrentThreadId());
            const std::wstring pending = result + suffix;
            DeleteFileW(pending.c_str());
            const ArchiveFormat format = DetectArchiveFormatFromPath(archive.c_str());
            const bool encryptedRar = entry.isEncrypted && format == ArchiveFormat::Rar;
            const bool extracted = encryptedRar
                ? RarNativeExtractEncryptedEntryToFile(archive.c_str(), entry.index, pending.c_str(), error)
                : ArchiveExtractEntryToFile(archive.c_str(), entry.index, pending.c_str(), error);
            if (!extracted || !ValidateFile(pending, entry.size))
            {
                if (extracted)
                    error = "Managed-cache extraction produced an unexpected file length";
                DeleteFileW(pending.c_str());
                break;
            }
            if (!MoveFileExW(pending.c_str(), result.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            {
                error = "Unable to publish managed-cache file";
                DeleteFileW(pending.c_str());
                break;
            }
            Touch(result);
            if (settings.cacheAutoCleanup)
                Cleanup(limit, result, settings.cacheAgeDays);
            ok = true;
        } while (false);

        ReleaseMutex(mutex);
        CloseHandle(mutex);
        if (!ok)
            result.clear();
        return ok;
    }
}
