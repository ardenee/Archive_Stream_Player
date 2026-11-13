#pragma once
// Shared cross-host background preparation of other tracks in a solid archive.
// The worker is pinned to its hosting plugin DLL until it finishes. It never
// displays password UI or extracts non-solid tracks that can stream directly.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include <algorithm>
#include "archive_unified_cache.h"
#include "archive_passwords.h"
#include "debug_log.h"

namespace ArchiveUnifiedWarmer
{
    struct Work
    {
        std::wstring archive;
        uint32_t selectedIndex = 0;
        ArchiveAdapterPreferences settings;
        HMODULE module = nullptr;
        uint64_t generation = 0;
    };

    inline std::atomic<uint64_t>& LatestGeneration()
    {
        static std::atomic<uint64_t> counter{0};
        return counter;
    }

    inline DWORD WINAPI Run(LPVOID param)
    {
        std::unique_ptr<Work> work(static_cast<Work*>(param));
        ArchivePasswordSetPromptAllowedForCurrentThread(false);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);

        {
        std::vector<ArchiveEntry> entries;
        std::string error;
        if (ArchiveListEntries(work->archive.c_str(), entries, error))
        {
            // Begin with the next audio track, then wrap to earlier tracks.
            std::stable_sort(entries.begin(), entries.end(), [&](const ArchiveEntry& a, const ArchiveEntry& b)
            {
                const auto priority = [&](const ArchiveEntry& e)
                {
                    return e.index > work->selectedIndex ? static_cast<uint64_t>(e.index) :
                        static_cast<uint64_t>(e.index) + (1ull << 32);
                };
                return priority(a) < priority(b);
            });

            for (const auto& entry : entries)
            {
                if (work->generation != LatestGeneration().load(std::memory_order_acquire))
                    break;
                if (entry.index == work->selectedIndex || entry.isFolder || !entry.isSupportedAudio ||
                    !entry.isSolid || !entry.size || entry.size > static_cast<uint64_t>(work->settings.cacheLimitMb) * ArchiveUnifiedCache::kMb)
                    continue;

                std::wstring cached;
                if (ArchiveUnifiedCache::Find(work->archive, entry, work->settings, cached))
                    continue;

                std::string extractError;
                if (ArchiveUnifiedCache::Prepare(work->archive, entry, work->settings, cached, extractError))
                    DebugLogWrite("INFO", "WARM", "ENTRY_CACHED", "index=%u | size=%llu", (unsigned)entry.index, (unsigned long long)entry.size);
                else
                    DebugLogWrite("WARN", "WARM", "ENTRY_FAIL", "index=%u | error=%s", (unsigned)entry.index, extractError.c_str());
            }
        }
        else
            DebugLogWrite("WARN", "WARM", "LIST_FAIL", "error=%s", error.c_str());
        }

        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
        HMODULE hostModule = work->module;
        work.reset();
        if (hostModule)
            FreeLibraryAndExitThread(hostModule, 0);
        return 0;
    }

    inline bool Queue(const std::wstring& archive, const ArchiveEntry& selected,
        const ArchiveAdapterPreferences& settings)
    {
        if (!settings.warmSolidArchives || settings.cacheLimitMb == 0 ||
            !selected.isSolid || archive.empty())
            return false;
        const ArchiveFormat format = DetectArchiveFormatFromPath(archive.c_str());
        if (format != ArchiveFormat::SevenZip && format != ArchiveFormat::Rar)
            return false;

        HMODULE module = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
            reinterpret_cast<LPCWSTR>(&Run), &module))
            return false;

        std::unique_ptr<Work> work(new (std::nothrow) Work);
        if (!work)
        {
            FreeLibrary(module);
            return false;
        }
        work->archive = archive;
        work->selectedIndex = selected.index;
        work->settings = settings;
        work->module = module;
        work->generation = LatestGeneration().fetch_add(1, std::memory_order_acq_rel) + 1;

        HANDLE thread = CreateThread(nullptr, 0, Run, work.get(), 0, nullptr);
        if (!thread)
        {
            FreeLibrary(module);
            return false;
        }
        work.release();
        CloseHandle(thread);
        return true;
    }
}
