#pragma once
#include <windows.h>
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "archive_common.h"

bool ArchiveCacheBuildPath(const char* internalName, mz_uint64 uncompressedSize, mz_uint32 crc32, bool hasCrc32, char* outPath, size_t outPathSize);
bool ArchiveCacheGetFolder(char* outFolder, size_t outFolderSize);
bool ArchiveCacheFileExists(const char* path, mz_uint64 expectedSize);
bool ArchiveCacheTouchFile(const char* path);
bool ArchiveCacheCleanup(unsigned long long maxCacheBytes);
bool ArchiveCacheCleanupByAge(unsigned int maxAgeDays);
bool ArchiveCacheClear();
bool ArchiveCacheWriteFile(const char* path, const void* data, size_t dataSize);
bool ArchiveCacheRecordFile(const char* cachePath, const char* archivePath, const char* internalName, const char* archiveType, const char* audioType, mz_uint64 uncompressedSize, mz_uint32 crc32, bool hasCrc32);
bool ArchiveExtractEntryToCacheFile(const char* archivePath, uint32_t index, const char* internalName, char* outCachePath, size_t outCachePathSize);

struct ArchiveCacheWarmResult
{
    uint32_t index = 0;
    char cachePath[MAX_PATH * 4]{};
    bool cached = false;
};

bool ArchiveCacheWarmSevenZipSolidEntries(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults);

typedef void (*ArchiveCacheWarmProgressCallback)(const char* archivePath, uint32_t index, const char* cachePath, void* userData);

bool ArchiveCacheWarmRarSolidEntries(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults);
bool ArchiveCacheWarmRarSolidEntriesWithProgress(const char* archivePath, const std::vector<ArchiveAudioEntry>& entries, std::vector<ArchiveCacheWarmResult>& outResults, ArchiveCacheWarmProgressCallback progressCallback, void* progressUserData);
void ArchiveCacheSetCleanupSettings(bool ageCleanupEnabled, uint32_t maxAgeDays, bool sizeCleanupEnabled, size_t maxSizeMb);
bool ArchiveFindExistingCacheFile(const char* archivePath, uint32_t index, const char* internalName, char* outCachePath, size_t outCachePathSize);