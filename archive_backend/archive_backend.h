#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "archive_types.h"

bool ArchiveListEntries(const wchar_t* archivePath,std::vector<ArchiveEntry>& entries,std::string& error);
bool ArchiveExtractEntryToMemory(const wchar_t* archivePath, uint32_t index, std::vector<unsigned char>& data, std::string& error);
bool ArchiveExtractEntryToFile(const wchar_t* archivePath, uint32_t index, const wchar_t* outputPath, std::string& error);
bool ArchiveOpenEntryStream(const wchar_t* archivePath, uint32_t index, ArchiveBackendStream& stream, std::string& error);
size_t ArchiveReadEntryStream(ArchiveBackendStream& stream, void* buffer, size_t bytesToRead);
bool ArchiveSeekEntryStream(ArchiveBackendStream& stream, uint64_t absolutePosition);
void ArchiveCloseEntryStream(ArchiveBackendStream& stream);
bool ArchiveEntryCanStream(const char* archivePath, unsigned int index);

