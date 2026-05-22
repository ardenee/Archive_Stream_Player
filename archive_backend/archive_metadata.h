#pragma once
#include <stddef.h>
#include <stdint.h>
#include <windows.h>
#include <shlobj.h>
#include <fstream>
#include <sstream>

#include "mf_audio_decoder.h"

bool ArchiveFormatVirtualTitle(const char* virtualPath, const char* baseTitle, char* outTitle, size_t outTitleSize);
bool GetVirtualArchiveEntryInfo(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
bool GetAudioDurationMsFromArchiveEntryCacheOnly(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs);
bool GetAudioDurationMsFromArchiveEntryBackground(const char* archivePath, uint32_t index, uint64_t uncompSize, const char* filename, int* outLengthMs);

bool GetVirtualArchiveEntryInfoCacheOnly(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
bool GetVirtualArchiveEntryInfoBackground(const char* file, char* outTitle, size_t outTitleSize, int* outLengthMs);
void ArchiveMetadataClearFingerprintCache();