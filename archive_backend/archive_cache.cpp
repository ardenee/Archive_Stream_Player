#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include <string>
#include <algorithm>
#include <ctype.h>

#include "archive_cache.h"
#include "archive_reader.h"
#include "archive_common.h"
#include "archive_format.h"
#include "debug_log.h"
#include "archive_backend.h"
#include "archive_7z_reader.h"
#include "archive_rar_libarchive.h"

static const char* CacheArchiveTypeFromPath(const char* archivePath);
static const char* CacheAudioTypeFromPath(const char* audioPath);

bool ArchiveFindExistingCacheFile(const char* archivePath, uint32_t index, const char* internalName, char* outCachePath, size_t outCachePathSize)
{
    if (!archivePath || !archivePath[0] || !outCachePath || outCachePathSize == 0)
    {
        DebugLogWrite("ERROR", "CACHE", "EXISTING_CHECK_FAIL", "archive=%s | index=%u | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    outCachePath[0] = '\0';

    uint64_t uncompSize = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    char entryName[1024];
    entryName[0] = '\0';

    if (!ArchiveGetEntryInfoEx(archivePath, index, entryName, sizeof(entryName), &uncompSize, &crc32, &hasCrc32))
    {
        DebugLogWrite("WARN", "CACHE", "EXISTING_CHECK_FAIL", "archive=%s | index=%u | reason=entry_info_failed", archivePath, (unsigned int)index);
        return false;
    }

    const char* nameForCache = internalName && internalName[0] ? internalName : entryName;

    if (!ArchiveCacheBuildPath(nameForCache, uncompSize, crc32, hasCrc32, outCachePath, outCachePathSize))
    {
        DebugLogWrite("WARN", "CACHE", "EXISTING_CHECK_FAIL", "archive=%s | index=%u | internal=%s | size=%llu | reason=build_path_failed", archivePath, (unsigned int)index, nameForCache ? nameForCache : "", (unsigned long long)uncompSize);
        return false;
    }

    if (!ArchiveCacheFileExists(outCachePath, uncompSize))
    {
        outCachePath[0] = '\0';
        return false;
    }

    ArchiveCacheTouchFile(outCachePath);
    ArchiveCacheRecordFile(outCachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);
    DebugLogWrite("INFO", "CACHE", "EXISTING_HIT", "archive=%s | index=%u | internal=%s | cache=%s | size=%llu | crc=%08X | has_crc=%d", archivePath, (unsigned int)index, nameForCache ? nameForCache : "", outCachePath, (unsigned long long)uncompSize, (unsigned int)crc32, hasCrc32 ? 1 : 0);

    return true;
}


static const char* CacheArchiveTypeFromPath(const char* archivePath)
{
    if (!archivePath || !archivePath[0])
        return "";

    wchar_t wideArchivePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
        return "Unknown";

    ArchiveFormat format = DetectArchiveFormatFromPath(wideArchivePath);
    return ArchiveFormatToString(format);
}

static const char* CacheAudioTypeFromPath(const char* audioPath)
{
    if (!audioPath || !audioPath[0])
        return "";

    AudioFormat format = GetAudioFormatFromPath(audioPath);

    switch (format)
    {
    case AUDIO_FORMAT_MP3: return "MP3";
    case AUDIO_FORMAT_WAV: return "WAV";
    case AUDIO_FORMAT_FLAC: return "FLAC";
    case AUDIO_FORMAT_OGG: return "OGG";
    case AUDIO_FORMAT_M4A: return "M4A";
    case AUDIO_FORMAT_AAC: return "AAC";
    case AUDIO_FORMAT_WMA: return "WMA";
    default: return "Audio";
    }
}


static bool s_cacheAgeCleanupEnabled = false;
static uint32_t s_cacheMaxAgeDays = 30;
static bool s_cacheSizeCleanupEnabled = false;
static size_t s_configuredCacheSizeMb = 1024;

void ArchiveCacheSetCleanupSettings(bool ageCleanupEnabled, uint32_t maxAgeDays, bool sizeCleanupEnabled, size_t maxSizeMb)
{
    s_cacheAgeCleanupEnabled = ageCleanupEnabled;
    s_cacheMaxAgeDays = maxAgeDays;
    s_cacheSizeCleanupEnabled = sizeCleanupEnabled;
    s_configuredCacheSizeMb = maxSizeMb;
}


static bool EnsureCacheFolder(char* outFolder, size_t outFolderSize)
{
    if (!outFolder || outFolderSize == 0)
    {
        // should ad dloggin here
        return false;
    }

    outFolder[0] = '\0';

    if (!ArchiveBuildPluginDataSubdirPath("Cache", outFolder, outFolderSize))
    {
        DebugLogWrite("ERROR", "CACHE", "FOLDER_FAIL", "reason=plugin_data_cache_subdir_failed");
        return false;
    }

    return true;
}

bool ArchiveExtractEntryToCacheFile(const char* archivePath, uint32_t index, const char* internalName, char* outCachePath, size_t outCachePathSize)
{
    if (!archivePath || !archivePath[0] || !outCachePath || outCachePathSize == 0)
    {
        DebugLogWrite("ERROR", "CACHE", "REQUEST_FAIL", "archive=%s | index=%u | reason=invalid_input", archivePath ? archivePath : "", (unsigned int)index);
        return false;
    }

    outCachePath[0] = '\0';

    uint64_t uncompSize = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    char entryName[1024];
    entryName[0] = '\0';

    if (!ArchiveGetEntryInfoEx(archivePath, index, entryName, sizeof(entryName), &uncompSize, &crc32, &hasCrc32))
    {
        DebugLogWrite("ERROR", "CACHE", "ENTRY_INFO_FAIL", "archive=%s | index=%u", archivePath, (unsigned int)index);
        return false;
    }

    const char* nameForCache = internalName && internalName[0] ? internalName : entryName;

    if (!ArchiveCacheBuildPath(nameForCache, uncompSize, crc32, hasCrc32, outCachePath, outCachePathSize))
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_PATH_FAIL", "archive=%s | index=%u | internal=%s | size=%llu", archivePath, (unsigned int)index, nameForCache, (unsigned long long)uncompSize);
        return false;
    }

    if (ArchiveCacheFileExists(outCachePath, uncompSize))
    {
        ArchiveCacheTouchFile(outCachePath);
        ArchiveCacheRecordFile(outCachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);
        return true;
    }

    if (s_cacheAgeCleanupEnabled)
    {
        if (!ArchiveCacheCleanupByAge(s_cacheMaxAgeDays))
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | index=%u | mode=age | max_age_days=%u", archivePath, (unsigned int)index, s_cacheMaxAgeDays);
    }

    if (s_cacheSizeCleanupEnabled && s_configuredCacheSizeMb > 0)
    {
        if (!ArchiveCacheCleanup((unsigned long long)s_configuredCacheSizeMb * 1024ULL * 1024ULL))
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | index=%u | mode=size | cache_mb=%zu", archivePath, (unsigned int)index, s_configuredCacheSizeMb);
    }

    if (uncompSize == 0 || uncompSize > (uint64_t)SIZE_MAX)
    {
        DebugLogWrite("ERROR", "CACHE", "INVALID_SIZE", "archive=%s | index=%u | size=%llu", archivePath, (unsigned int)index, (unsigned long long)uncompSize);
        return false;
    }

    if (IsSevenZipArchivePath(archivePath) || IsRarArchivePath(archivePath))
    {
        wchar_t wideArchivePath[MAX_PATH * 4];
        wchar_t wideCachePath[MAX_PATH * 4];

        if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
        {
            DebugLogWrite("ERROR", "CACHE", "PATH_CONVERT_FAIL", "archive=%s | index=%u | reason=archive_path", archivePath, (unsigned int)index);
            return false;
        }

        if (!CharPathToWidePath(outCachePath, wideCachePath, sizeof(wideCachePath) / sizeof(wideCachePath[0])))
        {
            DebugLogWrite("ERROR", "CACHE", "PATH_CONVERT_FAIL", "archive=%s | index=%u | cache=%s | reason=cache_path", archivePath, (unsigned int)index, outCachePath);
            return false;
        }

        std::string error;

        if (!ArchiveExtractEntryToFile(wideArchivePath, index, wideCachePath, error))
        {
            DebugLogWrite("ERROR", "CACHE", "BACKEND_EXTRACT_FAIL", "archive=%s | index=%u | cache=%s | error=%s", archivePath, (unsigned int)index, outCachePath, error.c_str());
            DeleteFileA(outCachePath);
            return false;
        }

        if (!ArchiveCacheFileExists(outCachePath, uncompSize))
        {
            DebugLogWrite("ERROR", "CACHE", "VERIFY_FAIL", "archive=%s | index=%u | cache=%s | expected=%llu", archivePath, (unsigned int)index, outCachePath, (unsigned long long)uncompSize);
            DeleteFileA(outCachePath);
            return false;
        }

        ArchiveCacheTouchFile(outCachePath);
        ArchiveCacheRecordFile(outCachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);
        return true;
    }

    std::vector<unsigned char> data;
    std::string error;

    if (!ArchiveExtractEntryToMemory(archivePath, index, data, error))
    {
        DebugLogWrite("ERROR", "CACHE", "MEMORY_EXTRACT_FAIL", "archive=%s | index=%u | size=%llu | error=%s", archivePath, (unsigned int)index, (unsigned long long)uncompSize, error.c_str());
        return false;
    }

    if (data.empty())
    {
        DebugLogWrite("ERROR", "CACHE", "MEMORY_EXTRACT_FAIL", "archive=%s | index=%u | reason=empty_data", archivePath, (unsigned int)index);
        return false;
    }

    if (!ArchiveCacheWriteFile(outCachePath, data.data(), data.size()))
    {
        DebugLogWrite("ERROR", "CACHE", "WRITE_FAIL", "archive=%s | index=%u | cache=%s | size=%zu", archivePath, (unsigned int)index, outCachePath, data.size());
        return false;
    }

    ArchiveCacheTouchFile(outCachePath);
    ArchiveCacheRecordFile(outCachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);
    return true;
}

// -----------------------------------------------------------------------------
// ArchiveCacheWarmSevenZipSolidEntries
//
// Builds managed cache paths for several solid 7z entries and asks the 7z backend
// to extract them in one archive session. This is the cache-layer part of the
// solid 7z cache warmer.
//
// Existing cache files are reused and recorded immediately. Missing cache files
// are sent to SevenZipExtractEntriesToFiles(), which keeps one 7z archive open and
// reuses the 7z SDK block buffer for entries in the same solid folder/block.
//
// Returns true if at least one requested entry is already cached or becomes
// cached. Individual entry failures are logged and reported through outResults.
// -----------------------------------------------------------------------------
bool ArchiveCacheWarmSevenZipSolidEntries(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults)
{
    outResults.clear();

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "CACHE", "SOLID_7Z_BATCH_FAIL", "reason=missing_archive_path");
        return false;
    }

    if (!IsSevenZipArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_FAIL", "archive=%s | reason=not_7z_archive", archivePath);
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_SKIP", "archive=%s | reason=no_entries", archivePath);
        return false;
    }

    if (s_cacheAgeCleanupEnabled)
    {
        if (!ArchiveCacheCleanupByAge(s_cacheMaxAgeDays))
        {
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | mode=age | max_age_days=%u | reason=solid_7z_batch_preclean", archivePath, s_cacheMaxAgeDays);
        }
    }

    if (s_cacheSizeCleanupEnabled && s_configuredCacheSizeMb > 0)
    {
        if (!ArchiveCacheCleanup((unsigned long long)s_configuredCacheSizeMb * 1024ULL * 1024ULL))
        {
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | mode=size | cache_mb=%zu | reason=solid_7z_batch_preclean", archivePath, s_configuredCacheSizeMb);
        }
    }

    wchar_t wideArchivePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
    {
        DebugLogWrite("ERROR", "CACHE", "SOLID_7Z_BATCH_FAIL", "archive=%s | reason=archive_path_convert_failed", archivePath);
        return false;
    }

    struct PendingCacheItem
    {
        uint32_t index = 0;
        uint64_t uncompSize = 0;
        uint32_t crc32 = 0;
        bool hasCrc32 = false;
        std::string internalName;
        std::string cachePath;
    };


    std::vector<SevenZipExtractFileRequest> requests;
    std::vector<PendingCacheItem> pendingItems;
    bool anyCached = false;

    DebugLogWrite("INFO", "CACHE", "SOLID_7Z_BATCH_START", "archive=%s | entries=%zu", archivePath, entries.size());

    for (const ArchiveAudioEntry& entry : entries)
    {
        ArchiveCacheWarmResult result;
        result.index = entry.index;
        result.cachePath[0] = '\0';
        result.cached = false;

        if (entry.name.empty())
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_SKIP", "archive=%s | index=%u | reason=empty_entry_name", archivePath, (unsigned int)entry.index);
            outResults.push_back(result);
            continue;
        }

        uint64_t uncompSize = 0;
        uint32_t crc32 = 0;
        bool hasCrc32 = false;
        char entryName[1024];
        entryName[0] = '\0';

        if (!ArchiveGetEntryInfoEx(archivePath, entry.index, entryName, sizeof(entryName), &uncompSize, &crc32, &hasCrc32))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | reason=entry_info_failed", archivePath, (unsigned int)entry.index, entry.name.c_str());
            outResults.push_back(result);
            continue;
        }

        const char* nameForCache = entry.name.c_str();

        if (!nameForCache || !nameForCache[0])
        {
            nameForCache = entryName;
        }

        char cachePath[MAX_PATH * 4];
        cachePath[0] = '\0';

        if (!ArchiveCacheBuildPath(nameForCache, uncompSize, crc32, hasCrc32, cachePath, sizeof(cachePath)))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | size=%llu | reason=build_cache_path_failed", archivePath, (unsigned int)entry.index, nameForCache ? nameForCache : "", (unsigned long long)uncompSize);
            outResults.push_back(result);
            continue;
        }

        strcpy_s(result.cachePath, sizeof(result.cachePath), cachePath);

        if (ArchiveCacheFileExists(cachePath, uncompSize))
        {
            ArchiveCacheTouchFile(cachePath);
            ArchiveCacheRecordFile(cachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);

            result.cached = true;
            outResults.push_back(result);
            anyCached = true;

            DebugLogWrite("INFO", "CACHE", "SOLID_7Z_BATCH_ITEM_HIT", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", archivePath, (unsigned int)entry.index, nameForCache, cachePath, (unsigned long long)uncompSize);
            continue;
        }

        wchar_t wideCachePath[MAX_PATH * 4];

        if (!CharPathToWidePath(cachePath, wideCachePath, sizeof(wideCachePath) / sizeof(wideCachePath[0])))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | cache=%s | reason=cache_path_convert_failed", archivePath, (unsigned int)entry.index, nameForCache, cachePath);
            outResults.push_back(result);
            continue;
        }

        SevenZipExtractFileRequest request;
        request.index = entry.index;
        request.outputPath = wideCachePath;
        request.expectedSize = uncompSize;

        PendingCacheItem pending;
        pending.index = entry.index;
        pending.uncompSize = uncompSize;
        pending.crc32 = crc32;
        pending.hasCrc32 = hasCrc32;
        pending.internalName = nameForCache;
        pending.cachePath = cachePath;

        try
        {
            requests.push_back(request);
            pendingItems.push_back(pending);
            outResults.push_back(result);
        }
        catch (const std::bad_alloc&)
        {
            DebugLogWrite("ERROR", "CACHE", "SOLID_7Z_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | reason=allocation_failed", archivePath, (unsigned int)entry.index, nameForCache);
            return anyCached;
        }
    }

    if (requests.empty())
    {
        DebugLogWrite("INFO", "CACHE", "SOLID_7Z_BATCH_DONE", "archive=%s | requested=%zu | extracted=0 | reason=nothing_missing", archivePath, entries.size());
        return anyCached;
    }

    std::vector<uint32_t> succeededIndexes;
    std::string error;

    if (!SevenZipExtractEntriesToFiles(wideArchivePath, requests, succeededIndexes, error))
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_BACKEND_FAIL", "archive=%s | requests=%zu | error=%s", archivePath, requests.size(), error.c_str());
    }

    for (const PendingCacheItem& pending : pendingItems)
    {
        bool extracted = std::find(succeededIndexes.begin(), succeededIndexes.end(), pending.index) != succeededIndexes.end();

        if (!extracted)
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | reason=backend_did_not_extract", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str());
            DeleteFileA(pending.cachePath.c_str());
            continue;
        }

        if (!ArchiveCacheFileExists(pending.cachePath.c_str(), pending.uncompSize))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_7Z_BATCH_ITEM_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | expected=%llu | reason=verify_failed", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str(), (unsigned long long)pending.uncompSize);
            DeleteFileA(pending.cachePath.c_str());
            continue;
        }

        ArchiveCacheTouchFile(pending.cachePath.c_str());
        ArchiveCacheRecordFile(pending.cachePath.c_str(), archivePath, pending.internalName.c_str(), CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(pending.internalName.c_str()), pending.uncompSize, pending.crc32, pending.hasCrc32);

        for (ArchiveCacheWarmResult& result : outResults)
        {
            if (result.index == pending.index)
            {
                strcpy_s(result.cachePath, sizeof(result.cachePath), pending.cachePath.c_str());
                result.cached = true;
                break;
            }
        }

        anyCached = true;

        DebugLogWrite("INFO", "CACHE", "SOLID_7Z_BATCH_ITEM_DONE", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str(), (unsigned long long)pending.uncompSize);
    }

    DebugLogWrite("INFO", "CACHE", "SOLID_7Z_BATCH_DONE", "archive=%s | requested=%zu | backend_requests=%zu | backend_succeeded=%zu", archivePath, entries.size(), requests.size(), succeededIndexes.size());

    return anyCached;
}

struct PendingCacheItem
{
    uint32_t index = 0;
    uint64_t uncompSize = 0;
    uint32_t crc32 = 0;
    bool hasCrc32 = false;
    std::string internalName;
    std::string cachePath;
};

struct RarSolidCacheProgressContext
{
    const char* archivePath = nullptr;
    const std::vector<ArchiveAudioEntry>* entries = nullptr;
    std::vector<ArchiveCacheWarmResult>* results = nullptr;
    const std::vector<PendingCacheItem>* pendingItems = nullptr;
    ArchiveCacheWarmProgressCallback progressCallback = nullptr;
    void* progressUserData = nullptr;
};

static const PendingCacheItem* FindRarPendingCacheItemByIndex(const std::vector<PendingCacheItem>& pendingItems, uint32_t index)
{
    for (const PendingCacheItem& pending : pendingItems)
    {
        if (pending.index == index)
        {
            return &pending;
        }
    }

    return nullptr;
}

static ArchiveCacheWarmResult* FindRarWarmResultByIndex(std::vector<ArchiveCacheWarmResult>& results, uint32_t index)
{
    for (ArchiveCacheWarmResult& result : results)
    {
        if (result.index == index)
        {
            return &result;
        }
    }

    return nullptr;
}

static void RarSolidCacheBackendProgress(uint32_t index, const wchar_t* outputPath, void* userData)
{
    RarSolidCacheProgressContext* context = (RarSolidCacheProgressContext*)userData;

    if (!context || !context->archivePath || !context->pendingItems || !context->results)
    {
        return;
    }

    const PendingCacheItem* pending = FindRarPendingCacheItemByIndex(*context->pendingItems, index);

    if (!pending)
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_RAR_PROGRESS_SKIP", "archive=%s | index=%u | reason=pending_item_not_found", context->archivePath, (unsigned int)index);
        return;
    }

    if (!ArchiveCacheFileExists(pending->cachePath.c_str(), pending->uncompSize))
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_RAR_PROGRESS_SKIP", "archive=%s | index=%u | cache=%s | expected=%llu | reason=cache_verify_failed", context->archivePath, (unsigned int)index, pending->cachePath.c_str(), (unsigned long long)pending->uncompSize);
        return;
    }

    ArchiveCacheTouchFile(pending->cachePath.c_str());
    ArchiveCacheRecordFile(pending->cachePath.c_str(), context->archivePath, pending->internalName.c_str(), CacheArchiveTypeFromPath(context->archivePath), CacheAudioTypeFromPath(pending->internalName.c_str()), pending->uncompSize, pending->crc32, pending->hasCrc32);

    ArchiveCacheWarmResult* result = FindRarWarmResultByIndex(*context->results, index);

    if (result)
    {
        strcpy_s(result->cachePath, sizeof(result->cachePath), pending->cachePath.c_str());
        result->cached = true;
    }

    DebugLogWrite("INFO", "CACHE", "SOLID_RAR_PROGRESS_DONE", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", context->archivePath, (unsigned int)index, pending->internalName.c_str(), pending->cachePath.c_str(), (unsigned long long)pending->uncompSize);

    if (context->progressCallback)
    {
        context->progressCallback(context->archivePath, index, pending->cachePath.c_str(), context->progressUserData);
    }

    if (context->progressCallback)
    {
        context->progressCallback(context->archivePath, index, pending->cachePath.c_str(), context->progressUserData);
    }
}

bool ArchiveCacheWarmRarSolidEntriesWithProgress(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults, ArchiveCacheWarmProgressCallback progressCallback, void* progressUserData)
{
    outResults.clear();

    if (!archivePath || !archivePath[0])
    {
        DebugLogWrite("ERROR", "CACHE", "SOLID_RAR_BATCH_FAIL", "reason=missing_archive_path");
        return false;
    }

    if (!IsRarArchivePath(archivePath))
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_FAIL", "archive=%s | reason=not_rar_archive", archivePath);
        return false;
    }

    if (entries.empty())
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_SKIP", "archive=%s | reason=no_entries", archivePath);
        return false;
    }

    if (s_cacheAgeCleanupEnabled)
    {
        if (!ArchiveCacheCleanupByAge(s_cacheMaxAgeDays))
        {
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | mode=age | max_age_days=%u | reason=solid_rar_batch_preclean", archivePath, s_cacheMaxAgeDays);
        }
    }

    if (s_cacheSizeCleanupEnabled && s_configuredCacheSizeMb > 0)
    {
        if (!ArchiveCacheCleanup((unsigned long long)s_configuredCacheSizeMb * 1024ULL * 1024ULL))
        {
            DebugLogWrite("WARN", "CACHE", "CLEANUP_FAIL", "archive=%s | mode=size | cache_mb=%zu | reason=solid_rar_batch_preclean", archivePath, s_configuredCacheSizeMb);
        }
    }

    wchar_t wideArchivePath[MAX_PATH * 4];

    if (!CharPathToWidePath(archivePath, wideArchivePath, sizeof(wideArchivePath) / sizeof(wideArchivePath[0])))
    {
        DebugLogWrite("ERROR", "CACHE", "SOLID_RAR_BATCH_FAIL", "archive=%s | reason=archive_path_convert_failed", archivePath);
        return false;
    }

    std::vector<LibarchiveRarExtractFileRequest> requests;
    std::vector<PendingCacheItem> pendingItems;
    bool anyCached = false;

    DebugLogWrite("INFO", "CACHE", "SOLID_RAR_BATCH_START", "archive=%s | entries=%zu", archivePath, entries.size());

    for (const ArchiveAudioEntry& entry : entries)
    {
        ArchiveCacheWarmResult result;
        result.index = entry.index;
        result.cachePath[0] = '\0';
        result.cached = false;

        if (entry.name.empty())
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | reason=empty_entry_name", archivePath, (unsigned int)entry.index);
            outResults.push_back(result);
            continue;
        }

        uint64_t uncompSize = 0;
        uint32_t crc32 = 0;
        bool hasCrc32 = false;
        char entryName[1024];
        entryName[0] = '\0';

        if (!ArchiveGetEntryInfoEx(archivePath, entry.index, entryName, sizeof(entryName), &uncompSize, &crc32, &hasCrc32))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | reason=entry_info_failed", archivePath, (unsigned int)entry.index, entry.name.c_str());
            outResults.push_back(result);
            continue;
        }

        const char* nameForCache = entry.name.c_str();

        if (!nameForCache || !nameForCache[0])
        {
            nameForCache = entryName;
        }

        char cachePath[MAX_PATH * 4];

        cachePath[0] = '\0';

        if (!ArchiveCacheBuildPath(nameForCache, uncompSize, crc32, hasCrc32, cachePath, sizeof(cachePath)))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | size=%llu | reason=build_cache_path_failed", archivePath, (unsigned int)entry.index, nameForCache ? nameForCache : "", (unsigned long long)uncompSize);
            outResults.push_back(result);
            continue;
        }

        strcpy_s(result.cachePath, sizeof(result.cachePath), cachePath);

        if (ArchiveCacheFileExists(cachePath, uncompSize))
        {
            ArchiveCacheTouchFile(cachePath);
            ArchiveCacheRecordFile(cachePath, archivePath, nameForCache, CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(nameForCache), uncompSize, crc32, hasCrc32);

            result.cached = true;
            outResults.push_back(result);
            anyCached = true;

            //DebugLogWrite("INFO", "CACHE", "SOLID_RAR_BATCH_ITEM_HIT", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", archivePath, (unsigned int)entry.index, nameForCache, cachePath, (unsigned long long)uncompSize);

            if (progressCallback)
            {
                //DebugLogWrite("INFO", "CACHE", "SOLID_RAR_PROGRESS_HIT", "archive=%s | index=%u | entry=%s | cache=%s", archivePath, (unsigned int)entry.index, nameForCache, cachePath);
                progressCallback(archivePath, entry.index, cachePath, progressUserData);
            }

            continue;
        }

        wchar_t wideCachePath[MAX_PATH * 4];

        if (!CharPathToWidePath(cachePath, wideCachePath, sizeof(wideCachePath) / sizeof(wideCachePath[0])))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | cache=%s | reason=cache_path_convert_failed", archivePath, (unsigned int)entry.index, nameForCache, cachePath);
            outResults.push_back(result);
            continue;
        }

        LibarchiveRarExtractFileRequest request;

        request.index = entry.index;

        request.outputPath = _wcsdup(wideCachePath);

        request.expectedSize = uncompSize;

        if (!request.outputPath)
        {
            DebugLogWrite("ERROR", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | reason=output_path_alloc_failed", archivePath, (unsigned int)entry.index, nameForCache);
            outResults.push_back(result);
            continue;
        }

        PendingCacheItem pending;
        pending.index = entry.index;
        pending.uncompSize = uncompSize;
        pending.crc32 = crc32;
        pending.hasCrc32 = hasCrc32;
        pending.internalName = nameForCache;
        pending.cachePath = cachePath;

        try
        {
            requests.push_back(request);
            pendingItems.push_back(pending);
            outResults.push_back(result);
        }
        catch (const std::bad_alloc&)
        {
            free((void*)request.outputPath);
            DebugLogWrite("ERROR", "CACHE", "SOLID_RAR_BATCH_ITEM_SKIP", "archive=%s | index=%u | entry=%s | reason=allocation_failed", archivePath, (unsigned int)entry.index, nameForCache);
            return anyCached;
        }
    }

    if (requests.empty())
    {
        DebugLogWrite("INFO", "CACHE", "SOLID_RAR_BATCH_DONE", "archive=%s | requested=%zu | extracted=0 | reason=nothing_missing", archivePath, entries.size());
        return anyCached;
    }

    std::vector<uint32_t> succeededIndexes;
    std::string error;

    RarSolidCacheProgressContext progressContext;
    progressContext.archivePath = archivePath;
    progressContext.entries = &entries;
    progressContext.results = &outResults;
    progressContext.pendingItems = &pendingItems;
    progressContext.progressCallback = progressCallback;
    progressContext.progressUserData = progressUserData;

    if (!LibarchiveRarExtractEntriesToFilesWithProgress(wideArchivePath, requests, succeededIndexes, RarSolidCacheBackendProgress, &progressContext, error))
    {
        DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_BACKEND_FAIL", "archive=%s | requests=%zu | error=%s", archivePath, requests.size(), error.c_str());
    }

    for (const LibarchiveRarExtractFileRequest& request : requests)
    {
        free((void*)request.outputPath);
    }

    for (const PendingCacheItem& pending : pendingItems)
    {
        bool extracted = std::find(succeededIndexes.begin(), succeededIndexes.end(), pending.index) != succeededIndexes.end();

        if (!extracted)
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | reason=backend_did_not_extract", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str());
            DeleteFileA(pending.cachePath.c_str());
            continue;
        }

        if (!ArchiveCacheFileExists(pending.cachePath.c_str(), pending.uncompSize))
        {
            DebugLogWrite("WARN", "CACHE", "SOLID_RAR_BATCH_ITEM_FAIL", "archive=%s | index=%u | entry=%s | cache=%s | expected=%llu | reason=verify_failed", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str(), (unsigned long long)pending.uncompSize);
            DeleteFileA(pending.cachePath.c_str());
            continue;
        }

        ArchiveCacheTouchFile(pending.cachePath.c_str());
        ArchiveCacheRecordFile(pending.cachePath.c_str(), archivePath, pending.internalName.c_str(), CacheArchiveTypeFromPath(archivePath), CacheAudioTypeFromPath(pending.internalName.c_str()), pending.uncompSize, pending.crc32, pending.hasCrc32);

        for (ArchiveCacheWarmResult& result : outResults)
        {
            if (result.index == pending.index)
            {
                strcpy_s(result.cachePath, sizeof(result.cachePath), pending.cachePath.c_str());
                result.cached = true;
                break;
            }
        }

        anyCached = true;

        DebugLogWrite("INFO", "CACHE", "SOLID_RAR_BATCH_ITEM_DONE", "archive=%s | index=%u | entry=%s | cache=%s | size=%llu", archivePath, (unsigned int)pending.index, pending.internalName.c_str(), pending.cachePath.c_str(), (unsigned long long)pending.uncompSize);
    }

    DebugLogWrite("INFO", "CACHE", "SOLID_RAR_BATCH_DONE", "archive=%s | requested=%zu | backend_requests=%zu | backend_succeeded=%zu", archivePath, entries.size(), requests.size(), succeededIndexes.size());

    return anyCached;
}

bool ArchiveCacheWarmRarSolidEntries(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults)
{
    return ArchiveCacheWarmRarSolidEntriesWithProgress(archivePath, entries, outResults, nullptr, nullptr);
}

bool ArchiveCacheGetFolder(char* outFolder, size_t outFolderSize)
{
    return EnsureCacheFolder(outFolder, outFolderSize);
}

static const char* CacheBaseNameFromPath(const char* path)
{
    if (!path || !path[0])
        return "";

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');

    const char* base = path;

    if (slash1 && slash1 + 1 > base)
        base = slash1 + 1;

    if (slash2 && slash2 + 1 > base)
        base = slash2 + 1;

    return base;
}

static bool ArchiveCacheGetIniPath(char* outIniPath, size_t outIniPathSize)
{
    if (!outIniPath || outIniPathSize == 0)
        return false;

    outIniPath[0] = '\0';

    char folder[MAX_PATH];

    if (!EnsureCacheFolder(folder, sizeof(folder)))
        return false;

    sprintf_s(
        outIniPath,
        outIniPathSize,
        "%s\\files.ini",
        folder
    );

    return true;
}

static void FileTimeToUnixString(char* out, size_t outSize)
{
    if (!out || outSize == 0)
        return;

    out[0] = '\0';

    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);

    ULARGE_INTEGER uli;
    uli.HighPart = ft.dwHighDateTime;
    uli.LowPart = ft.dwLowDateTime;

    unsigned long long unixTime =
        (uli.QuadPart - 116444736000000000ULL) / 10000000ULL;

    sprintf_s(
        out,
        outSize,
        "%llu",
        unixTime
    );
}

static void ArchiveCacheDeleteIniEntryForFile(const char* cacheFilePath)
{
    char iniPath[MAX_PATH * 4];

    if (!ArchiveCacheGetIniPath(iniPath, sizeof(iniPath)))
        return;

    const char* base = CacheBaseNameFromPath(cacheFilePath);

    if (!base || !base[0])
        return;

    WritePrivateProfileStringA(
        "Extracted_Files",
        base,
        NULL,
        iniPath
    );

    char section[512];

    sprintf_s(
        section,
        sizeof(section),
        "CacheFile_%s",
        base
    );

    WritePrivateProfileStringA(
        section,
        NULL,
        NULL,
        iniPath
    );
}

bool ArchiveCacheRecordFile(const char* cachePath, const char* archivePath, const char* internalName, const char* archiveType, const char* audioType, mz_uint64 uncompressedSize, mz_uint32 crc32, bool hasCrc32)
{
    if (!cachePath || !cachePath[0])
    {
        DebugLogWrite("ERROR", "CACHE", "INI_RECORD_FAIL", "reason=empty_cache_path");
        return false;
    }

    char iniPath[MAX_PATH * 4];

    if (!ArchiveCacheGetIniPath(iniPath, sizeof(iniPath)))
    {
        DebugLogWrite("ERROR", "CACHE", "INI_RECORD_FAIL", "cache=%s | reason=get_ini_path_failed", cachePath);
        return false;
    }

    const char* cacheBase = CacheBaseNameFromPath(cachePath);

    if (!cacheBase || !cacheBase[0])
    {
        DebugLogWrite("ERROR", "CACHE", "INI_RECORD_FAIL", "cache=%s | reason=empty_cache_base", cachePath);
        return false;
    }

    const char* originalName =
        internalName && internalName[0]
        ? internalName
        : cacheBase;

    /*
        Quick human lookup section requested:
        [Extracted_Files]
        37604889_A1B2C3D4.flac=TestTrack.flac
    */
    WritePrivateProfileStringA(
        "Extracted_Files",
        cacheBase,
        originalName,
        iniPath
    );

    char section[512];

    sprintf_s(
        section,
        sizeof(section),
        "CacheFile_%s",
        cacheBase
    );

    char value[128];

    WritePrivateProfileStringA(section, "OriginalName", originalName, iniPath);
    WritePrivateProfileStringA(section, "InternalPath", originalName, iniPath);
    WritePrivateProfileStringA(section, "ArchivePath", archivePath ? archivePath : "", iniPath);
    WritePrivateProfileStringA(section, "ArchiveType", archiveType ? archiveType : "", iniPath);
    WritePrivateProfileStringA(section, "AudioType", audioType ? audioType : "", iniPath);
    WritePrivateProfileStringA(section, "CacheFile", cacheBase, iniPath);

    sprintf_s(
        value,
        sizeof(value),
        "%llu",
        (unsigned long long)uncompressedSize
    );

    WritePrivateProfileStringA(section, "UncompressedSize", value, iniPath);

    if (hasCrc32)
    {
        sprintf_s(
            value,
            sizeof(value),
            "%08X",
            (unsigned int)crc32
        );

        WritePrivateProfileStringA(section, "CRC32", value, iniPath);
    }
    else
    {
        WritePrivateProfileStringA(section, "CRC32", "NOCRC", iniPath);
    }

    char now[64];
    FileTimeToUnixString(now, sizeof(now));

    WritePrivateProfileStringA(section, "LastAccessUnix", now, iniPath);

    char existingCreated[64];

    GetPrivateProfileStringA(
        section,
        "CreatedUnix",
        "",
        existingCreated,
        sizeof(existingCreated),
        iniPath
    );

    if (!existingCreated[0])
        WritePrivateProfileStringA(section, "CreatedUnix", now, iniPath);

    return true;
}

static const char* BaseNameFromPath(const char* path)
{
    if (!path || !path[0])
        return "cached_audio.bin";

    const char* slash1 = strrchr(path, '\\');
    const char* slash2 = strrchr(path, '/');

    const char* base = path;

    if (slash1 && slash1 + 1 > base)
        base = slash1 + 1;

    if (slash2 && slash2 + 1 > base)
        base = slash2 + 1;

    if (!base || !base[0])
        return "cached_audio.bin";

    return base;
}

static void MakeSafeFileName(const char* input, char* output, size_t outputSize)
{
    if (!output || outputSize == 0)
        return;

    output[0] = '\0';

    const char* base = BaseNameFromPath(input);

    size_t j = 0;

    for (size_t i = 0; base[i] && j + 1 < outputSize; ++i)
    {
        unsigned char c = (unsigned char)base[i];

        if (isalnum(c) || c == '.' || c == '-' || c == '_')
        {
            output[j++] = (char)c;
        }
        else
        {
            output[j++] = '_';
        }
    }

    output[j] = '\0';

    if (output[0] == '\0')
        strcpy_s(output, outputSize, "cached_audio.bin");
}

bool ArchiveCacheBuildPath( const char* internalName, mz_uint64 uncompressedSize, mz_uint32 crc32, bool hasCrc32, char* outPath, size_t outPathSize)
{
    if (!outPath || outPathSize == 0)
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_PATH_FAIL", "reason=invalid_output_buffer");
        return false;
    }

    outPath[0] = '\0';

    char folder[MAX_PATH];

    if (!EnsureCacheFolder(folder, sizeof(folder)))
    {
        DebugLogWrite("ERROR", "CACHE", "BUILD_PATH_FAIL", "reason=cache_folder_failed");
        return false;
    }

    const char* ext = ".bin";

    if (internalName && internalName[0])
    {
        const char* foundExt = strrchr(internalName, '.');

        if (foundExt && foundExt[1])
            ext = foundExt;
    }

    if (hasCrc32)
    {
        sprintf_s(
            outPath,
            outPathSize,
            "%s\\%llu_%08X%s",
            folder,
            (unsigned long long)uncompressedSize,
            (unsigned int)crc32,
            ext
        );
    }
    else
    {
        /*
            Fallback while CRC32 is not exposed for all archive backends.
            This is weaker and can collide if two different files have the
            same size and extension. For ZIP this should not be used once
            ArchiveGetEntryInfoEx() is wired to st.m_crc32.
        */
        sprintf_s(
            outPath,
            outPathSize,
            "%s\\%llu_NOCRC%s",
            folder,
            (unsigned long long)uncompressedSize,
            ext
        );
    }

    return true;
}

bool ArchiveCacheFileExists( const char* path, mz_uint64 expectedSize)
{
    if (!path || !path[0])
    {
        DebugLogWrite("WARN", "CACHE", "CHECK_FAIL", "reason=empty_path");
        return false;
    }

    WIN32_FILE_ATTRIBUTE_DATA data;

    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &data))
    {
        /*
            Normal cache miss. Do not log this; it is expected when a cache
            file has not been created yet.
        */
        return false;
    }

    if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    {
        DebugLogWrite("WARN", "CACHE", "MISS", "path=%s | reason=is_directory", path);
        return false;
    }

    ULARGE_INTEGER size;
    size.HighPart = data.nFileSizeHigh;
    size.LowPart = data.nFileSizeLow;

    if (size.QuadPart != expectedSize)
    {
        DebugLogWrite("WARN", "CACHE", "MISS", "path=%s | expected=%llu | actual=%llu | reason=size_mismatch", path, (unsigned long long)expectedSize, (unsigned long long)size.QuadPart);
        return false;
    }

    return true;
}

bool ArchiveCacheTouchFile(const char* path)
{
    if (!path || !path[0])
    {
        DebugLogWrite("WARN", "CACHE", "TOUCH_FAIL", "reason=empty_path");
        return false;
    }

    HANDLE hFile = CreateFileA(
        path,
        FILE_WRITE_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        NULL,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        DebugLogWrite("WARN", "CACHE", "TOUCH_FAIL", "path=%s | winerr=%lu | reason=open_failed", path, GetLastError());
        return false;
    }

    FILETIME now;
    GetSystemTimeAsFileTime(&now);

    BOOL ok = SetFileTime(
        hFile,
        NULL,   // creation time unchanged
        &now,   // last access time
        NULL    // last write time unchanged
    );

    CloseHandle(hFile);

    if (!ok)
        DebugLogWrite("WARN", "CACHE", "TOUCH_FAIL", "path=%s | winerr=%lu | reason=set_file_time_failed", path, GetLastError());

    return ok ? true : false;
}

struct ArchiveCacheFileInfo
{
    char path[MAX_PATH * 4];
    unsigned long long size;
    FILETIME lastAccessTime;
};

static int CompareFileTimeAscending(const FILETIME& a, const FILETIME& b)
{
    ULARGE_INTEGER ua;
    ua.HighPart = a.dwHighDateTime;
    ua.LowPart = a.dwLowDateTime;

    ULARGE_INTEGER ub;
    ub.HighPart = b.dwHighDateTime;
    ub.LowPart = b.dwLowDateTime;

    if (ua.QuadPart < ub.QuadPart)
        return -1;

    if (ua.QuadPart > ub.QuadPart)
        return 1;

    return 0;
}

bool ArchiveCacheCleanup(unsigned long long maxCacheBytes)
{
    if (maxCacheBytes == 0)
        return true;

    char folder[MAX_PATH];

    if (!EnsureCacheFolder(folder, sizeof(folder)))
    {
        DebugLogWrite("ERROR", "CACHE", "CLEANUP_FAIL", "reason=cache_folder_failed | mode=size");
        return false;
    }

    char searchPath[MAX_PATH * 4];

    sprintf_s(
        searchPath,
        sizeof(searchPath),
        "%s\\*.*",
        folder
    );

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);

    if (hFind == INVALID_HANDLE_VALUE)
        return true;

    std::vector<ArchiveCacheFileInfo> files;
    unsigned long long totalBytes = 0;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (_stricmp(fd.cFileName, "files.ini") == 0)
            continue;

        ArchiveCacheFileInfo info;
        memset(&info, 0, sizeof(info));

        sprintf_s(
            info.path,
            sizeof(info.path),
            "%s\\%s",
            folder,
            fd.cFileName
        );

        ULARGE_INTEGER size;
        size.HighPart = fd.nFileSizeHigh;
        size.LowPart = fd.nFileSizeLow;

        info.size = size.QuadPart;
        info.lastAccessTime = fd.ftLastAccessTime;
        totalBytes += info.size;
        files.push_back(info);
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    if (totalBytes <= maxCacheBytes)
        return true;

    std::sort(
        files.begin(),
        files.end(),
        [](const ArchiveCacheFileInfo& a, const ArchiveCacheFileInfo& b)
        {
            return CompareFileTimeAscending(
                a.lastAccessTime,
                b.lastAccessTime
            ) < 0;
        }
    );

    for (const ArchiveCacheFileInfo& file : files)
    {
        if (totalBytes <= maxCacheBytes)
            break;

        if (DeleteFileA(file.path))
        {
            ArchiveCacheDeleteIniEntryForFile(file.path);

            if (totalBytes >= file.size)
                totalBytes -= file.size;
            else
                totalBytes = 0;
        }
        else
        {
            DebugLogWrite("WARN", "CACHE", "DELETE_FAIL", "path=%s | bytes=%llu | winerr=%lu | reason=size_cleanup", file.path, file.size, GetLastError());
        }
    }

    return true;
}

bool ArchiveCacheCleanupByAge(unsigned int maxAgeDays)
{
    if (maxAgeDays == 0)
        return true;

    char folder[MAX_PATH];

    if (!EnsureCacheFolder(folder, sizeof(folder)))
    {
        DebugLogWrite("ERROR", "CACHE", "CLEANUP_FAIL", "reason=cache_folder_failed | mode=age");
        return false;
    }

    char searchPath[MAX_PATH * 4];

    sprintf_s(
        searchPath,
        sizeof(searchPath),
        "%s\\*.*",
        folder
    );

    FILETIME now;
    GetSystemTimeAsFileTime(&now);

    ULARGE_INTEGER nowUi;
    nowUi.HighPart = now.dwHighDateTime;
    nowUi.LowPart = now.dwLowDateTime;

    unsigned long long maxAge100ns =
        (unsigned long long)maxAgeDays *
        24ULL *
        60ULL *
        60ULL *
        10000000ULL;

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);

    if (hFind == INVALID_HANDLE_VALUE)
        return true;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        if (_stricmp(fd.cFileName, "files.ini") == 0)
            continue;

        ULARGE_INTEGER accessUi;
        accessUi.HighPart = fd.ftLastAccessTime.dwHighDateTime;
        accessUi.LowPart = fd.ftLastAccessTime.dwLowDateTime;

        if (nowUi.QuadPart > accessUi.QuadPart &&
            nowUi.QuadPart - accessUi.QuadPart > maxAge100ns)
        {
            char filePath[MAX_PATH * 4];

            sprintf_s(
                filePath,
                sizeof(filePath),
                "%s\\%s",
                folder,
                fd.cFileName
            );

            if (DeleteFileA(filePath))
            {
                ArchiveCacheDeleteIniEntryForFile(filePath);
            }
            else
            {
                DebugLogWrite("WARN", "CACHE", "DELETE_FAIL", "path=%s | max_age_days=%u | winerr=%lu | reason=age_cleanup", filePath, maxAgeDays, GetLastError());
            }
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);
    return true;
}

bool ArchiveCacheClear()
{
    char folder[MAX_PATH];

    if (!EnsureCacheFolder(folder, sizeof(folder)))
    {
        DebugLogWrite("ERROR", "CACHE", "CLEAR_FAIL", "reason=cache_folder_failed");
        return false;
    }

    char searchPath[MAX_PATH * 4];

    sprintf_s(
        searchPath,
        sizeof(searchPath),
        "%s\\*.*",
        folder
    );

    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);

    if (hFind == INVALID_HANDLE_VALUE)
        return true;

    bool allOk = true;

    do
    {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;

        char filePath[MAX_PATH * 4];

        sprintf_s(
            filePath,
            sizeof(filePath),
            "%s\\%s",
            folder,
            fd.cFileName
        );

        if (!DeleteFileA(filePath))
        {
            DebugLogWrite("WARN", "CACHE", "DELETE_FAIL", "path=%s | winerr=%lu | reason=manual_clear", filePath, GetLastError());
            allOk = false;
        }
    } while (FindNextFileA(hFind, &fd));

    FindClose(hFind);

    return allOk;
}

bool ArchiveCacheWriteFile( const char* path, const void* data, size_t dataSize)
{
    if (!path || !path[0] || !data || dataSize == 0)
    {
        DebugLogWrite("ERROR", "CACHE", "WRITE_FAIL", "path=%s | bytes=%zu | reason=invalid_input", path ? path : "", dataSize);
        return false;
    }

    HANDLE hFile = CreateFileA(
        path,
        GENERIC_WRITE,
        0,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile == INVALID_HANDLE_VALUE)
    {
        DebugLogWrite("ERROR", "CACHE", "WRITE_FAIL", "path=%s | bytes=%zu | winerr=%lu | reason=create_file_failed", path, dataSize, GetLastError());
        return false;
    }

    const unsigned char* p = (const unsigned char*)data;
    size_t remaining = dataSize;

    bool ok = true;

    while (remaining > 0)
    {
        DWORD chunk = remaining > 1024 * 1024
            ? 1024 * 1024
            : (DWORD)remaining;

        DWORD written = 0;

        if (!WriteFile(hFile, p, chunk, &written, NULL) || written != chunk)
        {
            DebugLogWrite("ERROR", "CACHE", "WRITE_FAIL", "path=%s | requested=%lu | written=%lu | winerr=%lu | reason=write_chunk_failed", path, chunk, written, GetLastError());
            ok = false;
            break;
        }

        p += written;
        remaining -= written;
    }

    CloseHandle(hFile);

    if (!ok)
    {
        DebugLogWrite("ERROR", "CACHE", "WRITE_FAIL", "path=%s | bytes=%zu | reason=write_failed", path, dataSize);
        DeleteFileA(path);
    }

    return ok;
}