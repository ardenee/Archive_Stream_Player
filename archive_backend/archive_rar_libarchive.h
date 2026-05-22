#pragma once

#include "archive_types.h"

#include <cstdint>
#include <string>
#include <vector>

struct LibarchiveRarExtractFileRequest
{
    uint32_t index = 0;
    const wchar_t* outputPath = nullptr;
    uint64_t expectedSize = 0;
};

typedef void (*LibarchiveRarExtractProgressCallback)(uint32_t index, const wchar_t* outputPath, void* userData);

bool LibarchiveRarListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
bool LibarchiveRarExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
bool LibarchiveRarExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);
bool LibarchiveRarExtractEntriesToFiles(const wchar_t* archivePath, const std::vector<LibarchiveRarExtractFileRequest>& requests, std::vector<uint32_t>& succeededIndexes, std::string& error);
bool LibarchiveRarExtractEntriesToFilesWithProgress( const wchar_t* archivePath, const std::vector<LibarchiveRarExtractFileRequest>& requests, std::vector<uint32_t>& succeededIndexes, LibarchiveRarExtractProgressCallback progressCallback, void* progressUserData, std::string& error);
bool LibarchiveRarOpenEntryStream(const wchar_t* archivePath, uint32_t index, void** outStream, uint64_t* outEntrySize, std::string& error);
size_t LibarchiveRarReadEntryStream(void* streamHandle, void* buffer, size_t bytesToRead, std::string& error);
bool LibarchiveRarSeekEntryStream(void* streamHandle, uint64_t absolutePosition, std::string& error);
void LibarchiveRarCloseEntryStream(void* streamHandle);