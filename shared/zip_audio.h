#pragma once
#include <vector>
#include <windows.h>
#include "archive_common.h"

bool GetSortedAudioEntriesInZip(const char* zipPath,std::vector<ArchiveAudioEntry>& outEntries);
bool FindFirstAudioInZip(const char* zipPath, mz_uint* outIndex, char* outName, size_t outNameSize, mz_uint64* outUncompressedSize);
//bool GetVirtualArchiveEntryInfo(const char* file, char* outTitle, size_t outTitleSize,int* outLengthMs);
bool RequestZipExpandFromGeneralPlugin(HWND hwndWinamp, const char* zipPath);