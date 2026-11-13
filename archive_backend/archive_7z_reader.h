#pragma once
#include "archive_types.h"

#include <cstdint>
#include <string>
#include <vector>

bool SevenZipListEntries(const wchar_t* archivePath, std::vector<ArchiveEntry>& entries, std::string& error);
bool SevenZipExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
bool SevenZipExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);

struct SevenZipExtractFileRequest
{
    uint32_t index = 0; 
    std::wstring outputPath; 
    uint64_t expectedSize = 0;
};

bool SevenZipExtractEntriesToFiles(const wchar_t* archivePath, const std::vector<SevenZipExtractFileRequest>& requests, std::vector<uint32_t>& outSucceededIndexes,  std::string& error);
bool SevenZipOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error, const ArchiveStreamCancellationToken* cancellation = nullptr);
size_t SevenZipReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead);
bool SevenZipSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition);
void SevenZipCloseEntryStream(ArchiveBackendStream& stream);