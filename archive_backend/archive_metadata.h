#pragma once
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

#include "mf_audio_decoder.h"
#include "archive_types.h"

bool ArchiveFormatVirtualTitle(const char* virtualPath, const char* baseTitle, char* outTitle, size_t outTitleSize);
bool GetVirtualArchiveEntryInfo(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
bool GetAudioDurationMsFromArchiveEntryCacheOnly(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs);
bool GetAudioDurationMsFromArchiveEntryBackground(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs);
// Read metadata only from a cache file already prepared for playback/warming.
// Uses the archive header's CRC and size as the persistent duration key.
bool GetAudioDurationMsFromPreparedCacheFile(const wchar_t* cachedPath, const ArchiveEntry& entry, int* outLengthMs);
bool GetAudioDurationMsFromPreparedMemory(const unsigned char* bytes, size_t size, const ArchiveEntry& entry, int* outLengthMs);

bool GetVirtualArchiveEntryInfoCacheOnly(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
bool GetVirtualArchiveEntryInfoBackground(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
void ArchiveMetadataClearFingerprintCache();